//! The apply engine: runs a plan against DFU, trusting none of it.
//!
//! What a plan can and cannot do, whoever sent it:
//!
//! - Nothing is touched before the target's descriptor passes: seal (signature) over its exact bytes first, then
//!   scheme, product, geometry, length and floor. The header's own fields (geometry, step count, body length)
//!   are outside the seal: they can only make the plan refused, and are bounded by the geometry.
//! - A step is exactly one encoded step, a WRITE's data exactly one chunk, and the steps add up to exactly the
//!   step count and the body length of the header.
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
use crate::descriptor::{Expect, ImageDescriptor, ImageError, Trust, open_descriptor, verify_image};
use crate::ecc::{EccError, ReadFault, try_read};
use crate::geometry::FlashGeometry;
use crate::hash::{ChunkHash, is_blank};
use crate::plan::{
    PLAN_HEADER_LEN, PlanError, Step, StepView, read_header_fixed, read_step, step_len, step_len_from_head,
};

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
    /// A body longer than any plan for this geometry needs, or steps that do not add up to the body length the
    /// header announced.
    BodyLength,
    /// Fewer steps than the header announced.
    Incomplete,
    /// A step is not after the one before it.
    OutOfOrder,
    /// A step names a chunk or block outside the image.
    OutOfRange,
    /// The bytes handed in are not exactly one step, or a WRITE's data is not exactly one chunk.
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
    descriptor: ImageDescriptor,
    /// Lowest position the next step may have: twice the chunk index, plus one for a program.
    next_position: u32,
    steps_left: u32,
    body_left: u32,
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
        let header = read_header_fixed(header).map_err(ApplyError::Plan)?;
        if header.geometry_id != geometry.id() {
            return Err(ApplyError::GeometryMismatch);
        }
        if header.step_count > geometry.blocks() + geometry.chunks() {
            return Err(ApplyError::TooManySteps);
        }
        if header.body_len as u64 > max_body_len(geometry) {
            return Err(ApplyError::BodyLength);
        }
        let expect = Expect {
            product_id,
            geometry_id: geometry.id(),
            image_len: geometry.image_len(),
            floor,
        };
        let descriptor = open_descriptor(&header.descriptor, trust, &expect).map_err(ApplyError::Image)?;
        if !backend.boot_state().map_err(|_| ApplyError::Backend)?.may_stage() {
            return Err(ApplyError::NotConfirmed);
        }
        Ok(Self {
            geometry,
            trust: *trust,
            expect,
            descriptor,
            next_position: 0,
            steps_left: header.step_count,
            body_left: header.body_len,
        })
    }

    /// Bytes of the step that starts with `head`, for a receiver that frames steps out of a stream: `Ok(None)`
    /// until `head` holds the step's head ([`MAX_STEP_HEAD_LEN`](crate::plan::MAX_STEP_HEAD_LEN) bytes always
    /// do). Refuses a WRITE whose data is not one chunk before a byte of it needs a buffer.
    pub fn step_len(&self, head: &[u8]) -> Result<Option<usize>, ApplyError> {
        let Some(len) = step_len_from_head(head).map_err(ApplyError::Plan)? else {
            return Ok(None);
        };
        if head[0] == Step::WRITE_TAG && len != write_len(self.geometry) {
            return Err(ApplyError::BadData);
        }
        Ok(Some(len))
    }

    /// Steps the plan still owes.
    pub fn steps_left(&self) -> u32 {
        self.steps_left
    }

    /// Runs one step: `step` holds exactly one encoded step, a WRITE with its data. `buf` is scratch of one
    /// chunk.
    pub fn step<A, D>(&mut self, step: &[u8], active: &mut A, dfu: &mut D, buf: &mut [u8]) -> Result<(), ApplyError>
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
        let view = read_step(step).map_err(ApplyError::Plan)?;
        let len = step_len(&view);
        if len != step.len() {
            return Err(ApplyError::BadData);
        }
        if len as u64 > self.body_left as u64 {
            return Err(ApplyError::BodyLength);
        }
        let step = match view {
            StepView::Erase(e) => Op::Erase { block: e.block() },
            StepView::Copy(c) => Op::Copy {
                dst: c.dst(),
                src: c.src(),
                hash: ChunkHash(c.hash()),
            },
            StepView::Write(w) => {
                let data = w.data().as_bytes();
                if data.len() != chunk {
                    return Err(ApplyError::BadData);
                }
                Op::Write {
                    dst: w.dst(),
                    data,
                    hash: ChunkHash(w.hash()),
                }
            }
        };
        let position = match step {
            Op::Erase { block } => {
                if block as u32 >= geometry.blocks() {
                    return Err(ApplyError::OutOfRange);
                }
                block as u32 * geometry.chunks_per_block() * 2
            }
            Op::Copy { dst, src, .. } => {
                if dst as u32 >= geometry.chunks() || src as u32 >= geometry.chunks() {
                    return Err(ApplyError::OutOfRange);
                }
                dst as u32 * 2 + 1
            }
            Op::Write { dst, .. } => {
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
            Op::Erase { block } => {
                let from = block as u32 * geometry.erase;
                dfu.erase(from, from + geometry.erase).map_err(|_| ApplyError::Flash)?;
            }
            Op::Copy { dst, src, hash } => {
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
            Op::Write { dst, data, hash } => {
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
        self.body_left -= len as u32;
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
        if self.body_left != 0 {
            return Err(ApplyError::BodyLength);
        }
        let descriptor = verify_image(dfu, &self.trust, &self.expect, buf).map_err(ApplyError::Image)?;
        if descriptor != self.descriptor {
            return Err(ApplyError::WrongImage);
        }
        backend.mark_staged().map_err(|_| ApplyError::Backend)?;
        Ok(descriptor)
    }
}

/// A step, its fields read from the view.
enum Op<'a> {
    Erase { block: u16 },
    Copy { dst: u16, src: u16, hash: ChunkHash },
    Write { dst: u16, data: &'a [u8], hash: ChunkHash },
}

/// Bytes of a WRITE of one chunk.
fn write_len(geometry: &FlashGeometry) -> usize {
    1 + Step::WRITE_HEAD_LEN + geometry.chunk as usize
}

/// The longest body a plan for `geometry` can need: every block erased, every chunk written.
fn max_body_len(geometry: &FlashGeometry) -> u64 {
    geometry.blocks() as u64 * (1 + Step::ERASE_HEAD_LEN) as u64 + geometry.chunks() as u64 * write_len(geometry) as u64
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
