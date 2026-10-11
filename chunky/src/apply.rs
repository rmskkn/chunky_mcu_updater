//! The apply engine: runs a plan against DFU, trusting none of it.
//!
//! What a plan can and cannot do, whoever sent it:
//!
//! - Nothing is touched before the target's descriptor passes: seal (signature), product, geometry, floor.
//! - It writes DFU only, and only the part an image occupies. ACTIVE is handed in as a read-only flash.
//! - Steps come in ascending order, an erase before the programs of its block. That bounds the work (each
//!   block is erased at most once, each chunk programmed at most once) and makes a plan with a COPY whose
//!   source was already overwritten impossible to express: sources are in ACTIVE, which no step can write.
//! - A chunk is programmed only when it is blank, so no write unit is ever programmed twice.
//! - A COPY source and WRITE data must have the hash the step names, and the chunk is read back afterwards.
//! - The image is switched to only after the whole of it verifies against the descriptor announced up front.
//!
//! A step that breaks a rule stops the plan with a named error. What is already in DFU stays: the next manifest
//! tells the server where things stand.

use embedded_storage::nor_flash::{NorFlash, ReadNorFlash};

use crate::backend::BootBackend;
use crate::descriptor::{DESCRIPTOR_LEN, Expect, ImageDescriptor, ImageError, Trust, verify_image};
use crate::ecc::{EccError, ReadFault, try_read};
use crate::geometry::FlashGeometry;
use crate::hash::{ChunkHash, is_blank};
use crate::plan::{PLAN_HEADER_LEN, PlanError, PlanHeader, STEP_LEN, Step};

/// Why a plan was stopped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ApplyError {
    /// The device is on a trial boot or has an update staged: DFU must not be written now.
    NotConfirmed,
    /// The plan is malformed.
    Plan(PlanError),
    /// The plan was made for another flash layout (`GEOMETRY_MISMATCH`).
    GeometryMismatch,
    /// The target image is refused.
    Image(ImageError),
    /// More steps than the header announced, or more than any plan for this geometry needs.
    TooManySteps,
    /// Fewer steps than the header announced.
    Incomplete,
    /// A step is not after the one before it.
    OutOfOrder,
    /// A step names a chunk or block outside the image.
    OutOfRange,
    /// A WRITE came without one chunk of data, or another step with data.
    BadData,
    /// The data of a WRITE does not have the hash the step names.
    DataMismatch,
    /// The source of a COPY does not have the hash the step names.
    SourceMismatch,
    /// The source of a COPY faults on ECC.
    SourceUnreadable,
    /// The chunk to program is not blank.
    NotBlank,
    /// The chunk does not read back as programmed.
    VerifyFailed,
    /// The staged image is not the one the plan announced.
    WrongImage,
    /// The boot backend failed.
    Backend,
    /// The flash failed.
    Flash,
}

/// Runs one plan. Create it with [`begin`](Self::begin), feed every step, then [`finish`](Self::finish).
pub struct Applier<'g> {
    geometry: &'g FlashGeometry,
    trust: Trust,
    expect: Expect,
    descriptor: [u8; DESCRIPTOR_LEN],
    /// Lowest position the next step may have: twice the chunk index, plus one for a program.
    next_position: u32,
    steps_left: u32,
}

impl<'g> Applier<'g> {
    /// Checks the plan header and the target's descriptor. Writes nothing.
    pub fn begin<B: BootBackend>(
        geometry: &'g FlashGeometry,
        header: &[u8; PLAN_HEADER_LEN],
        trust: &Trust,
        product_id: u32,
        floor: u32,
        backend: &mut B,
    ) -> Result<Self, ApplyError> {
        let header = PlanHeader::decode(header).map_err(ApplyError::Plan)?;
        if header.geometry_id != geometry.id() {
            return Err(ApplyError::GeometryMismatch);
        }
        if header.step_count > geometry.blocks() + geometry.chunks() {
            return Err(ApplyError::TooManySteps);
        }
        let expect = Expect {
            product_id,
            geometry_id: geometry.id(),
            image_len: geometry.image_len(),
            floor,
        };
        let descriptor = ImageDescriptor::parse(&header.descriptor).map_err(ApplyError::Image)?;
        descriptor.check(trust, &expect).map_err(ApplyError::Image)?;
        if !backend.boot_state().map_err(|_| ApplyError::Backend)?.may_stage() {
            return Err(ApplyError::NotConfirmed);
        }
        Ok(Self {
            geometry,
            trust: *trust,
            expect,
            descriptor: header.descriptor,
            next_position: 0,
            steps_left: header.step_count,
        })
    }

    /// Steps the plan still owes.
    pub fn steps_left(&self) -> u32 {
        self.steps_left
    }

    /// Runs one step. `data` is the chunk that follows a WRITE; `buf` is scratch of one chunk.
    pub fn step<A, D>(
        &mut self,
        step: &[u8; STEP_LEN],
        data: Option<&[u8]>,
        active: &mut A,
        dfu: &mut D,
        buf: &mut [u8],
    ) -> Result<(), ApplyError>
    where
        A: ReadNorFlash,
        A::Error: EccError,
        D: NorFlash,
        D::Error: EccError,
    {
        let geometry = self.geometry;
        let chunk = geometry.chunk as usize;
        assert!(buf.len() >= chunk);
        let buf = &mut buf[..chunk];

        if self.steps_left == 0 {
            return Err(ApplyError::TooManySteps);
        }
        let step = Step::decode(step).map_err(ApplyError::Plan)?;
        if step.has_data() != data.is_some() || data.is_some_and(|d| d.len() != chunk) {
            return Err(ApplyError::BadData);
        }
        let position = match step {
            Step::Erase { block } => {
                if block as u32 >= geometry.blocks() {
                    return Err(ApplyError::OutOfRange);
                }
                block as u32 * geometry.chunks_per_block() * 2
            }
            Step::Copy { dst, src, .. } => {
                if dst as u32 >= geometry.chunks() || src as u32 >= geometry.chunks() {
                    return Err(ApplyError::OutOfRange);
                }
                dst as u32 * 2 + 1
            }
            Step::Write { dst, .. } => {
                if dst as u32 >= geometry.chunks() {
                    return Err(ApplyError::OutOfRange);
                }
                dst as u32 * 2 + 1
            }
        };
        if position < self.next_position {
            return Err(ApplyError::OutOfOrder);
        }

        match step {
            Step::Erase { block } => {
                let from = block as u32 * geometry.erase;
                dfu.erase(from, from + geometry.erase).map_err(|_| ApplyError::Flash)?;
            }
            Step::Copy { dst, src, hash } => {
                let at = dst as u32 * geometry.chunk;
                require_blank(dfu, at, buf)?;
                match try_read(active, src as u32 * geometry.chunk, buf) {
                    Ok(()) => {}
                    Err(ReadFault::Ecc) => return Err(ApplyError::SourceUnreadable),
                    Err(ReadFault::Flash(_)) => return Err(ApplyError::Flash),
                }
                if ChunkHash::of(buf) != hash {
                    return Err(ApplyError::SourceMismatch);
                }
                dfu.write(at, buf).map_err(|_| ApplyError::Flash)?;
                read_back(dfu, at, buf, hash)?;
            }
            Step::Write { dst, hash } => {
                let data = data.ok_or(ApplyError::BadData)?;
                if ChunkHash::of(data) != hash {
                    return Err(ApplyError::DataMismatch);
                }
                let at = dst as u32 * geometry.chunk;
                require_blank(dfu, at, buf)?;
                dfu.write(at, data).map_err(|_| ApplyError::Flash)?;
                read_back(dfu, at, buf, hash)?;
            }
        }
        self.next_position = position + 1;
        self.steps_left -= 1;
        Ok(())
    }

    /// Verifies the whole staged image and tells the backend to switch to it at the next reset.
    pub fn finish<D, B>(self, dfu: &mut D, backend: &mut B, buf: &mut [u8]) -> Result<ImageDescriptor, ApplyError>
    where
        D: NorFlash,
        D::Error: EccError,
        B: BootBackend,
    {
        if self.steps_left != 0 {
            return Err(ApplyError::Incomplete);
        }
        let descriptor = verify_image(dfu, &self.trust, &self.expect, buf).map_err(ApplyError::Image)?;
        if descriptor.to_bytes() != self.descriptor {
            return Err(ApplyError::WrongImage);
        }
        backend.mark_staged().map_err(|_| ApplyError::Backend)?;
        Ok(descriptor)
    }
}

fn require_blank<D>(dfu: &mut D, at: u32, buf: &mut [u8]) -> Result<(), ApplyError>
where
    D: NorFlash,
    D::Error: EccError,
{
    match try_read(dfu, at, buf) {
        Ok(()) if is_blank(buf) => Ok(()),
        Ok(()) | Err(ReadFault::Ecc) => Err(ApplyError::NotBlank),
        Err(ReadFault::Flash(_)) => Err(ApplyError::Flash),
    }
}

fn read_back<D>(dfu: &mut D, at: u32, buf: &mut [u8], hash: ChunkHash) -> Result<(), ApplyError>
where
    D: NorFlash,
    D::Error: EccError,
{
    match try_read(dfu, at, buf) {
        Ok(()) if ChunkHash::of(buf) == hash => Ok(()),
        Ok(()) | Err(ReadFault::Ecc) => Err(ApplyError::VerifyFailed),
        Err(ReadFault::Flash(_)) => Err(ApplyError::Flash),
    }
}
