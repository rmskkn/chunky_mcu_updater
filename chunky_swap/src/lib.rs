//! chunky's boot backend on embassy-boot: swap ACTIVE and DFU, one trial boot, revert.
//!
//! embassy-boot does the power-fail-safe swap and revert and is used unmodified. This crate adds what it lacks:
//!
//! - [`boot`], the bootloader's decision: a staged image is verified before the swap starts, ACTIVE is verified
//!   on every boot (signature, product, geometry, version floor), a trial image that does not verify is reverted
//!   on the spot, and the floor is raised once an image is confirmed.
//! - [`SwapUpdater`], the application's [`BootBackend`]: mark staged, confirm, and tell "staged" from "trial".
//! - Every read goes through [`Masked`], so a word torn by a power cut on ECC flash reads as zero instead of
//!   faulting: a torn progress word counts as written, a torn magic as absent.
//! - Every erase of the state partition goes through [`Guarded`], which first writes zeros over the magic.
//!   embassy-boot erases the state page while it still says "swap"; an erase cut by power loss can then leave
//!   the magic in place with the progress words half gone, and the next boot would run swap steps again over
//!   partitions that are already swapped. With the magic zeroed first, a torn erase always reads as "boot".
#![no_std]
#![forbid(unsafe_code)]
#![warn(missing_docs)]

use chunky::descriptor::{Expect, ImageDescriptor, ImageError, Trust, verify_image};
use chunky::ecc::{EccError, Masked};
use chunky::floor::{FloorError, raise_floor, read_floor};
use chunky::geometry::FlashGeometry;
use chunky::{BootBackend, BootState};
use embassy_boot::{BlockingFirmwareState, BootLoader, BootLoaderConfig, State};
use embedded_storage::nor_flash::{ErrorType, NorFlash, ReadNorFlash};

/// Largest write unit this crate handles (H7: 32 bytes).
pub const MAX_WRITE_SIZE: usize = 32;

/// The state partition as embassy-boot gets it: ECC faults masked, and the magic zeroed before any erase.
#[derive(Clone)]
pub struct Guarded<S>(Masked<S>);

impl<S> Guarded<S> {
    /// Wraps the state partition.
    pub fn new(state: S) -> Self {
        Self(Masked(state))
    }
}

impl<S: ErrorType> ErrorType for Guarded<S> {
    type Error = S::Error;
}

impl<S> ReadNorFlash for Guarded<S>
where
    S: NorFlash,
    S::Error: EccError,
{
    const READ_SIZE: usize = S::READ_SIZE;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        self.0.read(offset, bytes)
    }

    fn capacity(&self) -> usize {
        self.0.capacity()
    }
}

impl<S> NorFlash for Guarded<S>
where
    S: NorFlash,
    S::Error: EccError,
{
    const WRITE_SIZE: usize = S::WRITE_SIZE;
    const ERASE_SIZE: usize = S::ERASE_SIZE;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        if from == 0 && to > 0 {
            // Zeros go over anything on every family (on H7 the word may fault afterwards; it reads as zero
            // through the mask, and the erase clears it).
            assert!(S::WRITE_SIZE <= MAX_WRITE_SIZE);
            self.0.write(0, &[0; MAX_WRITE_SIZE][..S::WRITE_SIZE])?;
        }
        self.0.erase(from, to)
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        self.0.write(offset, bytes)
    }
}

/// The flash failed (or, in the simulator, power was cut). Nothing can be decided: reset and try again.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BootFault;

/// What the bootloader needs to know about the device.
#[derive(Clone, Copy, Debug)]
pub struct BootConfig<'a> {
    /// The flash layout.
    pub geometry: &'a FlashGeometry,
    /// What images this device accepts: a release key, or (opt-out build) any image with the right hashes.
    pub trust: Trust,
    /// This device's product.
    pub product_id: u32,
}

/// What the bootloader decided.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootOutcome {
    /// Jump to ACTIVE: it holds this verified image.
    Run {
        /// The image in ACTIVE.
        image: ImageDescriptor,
        /// `Trial` when the image must confirm, `Reverted` right after a revert, else `Confirmed`.
        state: BootState,
        /// Why a staged or trial image was refused on this boot, if one was.
        rejected: Option<ImageError>,
    },
    /// ACTIVE does not verify and there is nothing to revert to. Do not jump; wait for a wired reflash.
    Recovery {
        /// Why ACTIVE is refused.
        reason: ImageError,
    },
}

/// True once embassy-boot has started (or finished) a swap: from then on DFU no longer holds the staged image.
fn swap_started<S>(state: &mut Masked<S>) -> Result<bool, BootFault>
where
    S: NorFlash,
    S::Error: EccError,
{
    assert!(S::WRITE_SIZE <= MAX_WRITE_SIZE);
    let mut word = [0; MAX_WRITE_SIZE];
    let word = &mut word[..S::WRITE_SIZE];
    // Same reading as embassy-boot: word 1 is "progress valid" while erased, word 2 is the first progress index.
    state.read(S::WRITE_SIZE as u32, word).map_err(|_| BootFault)?;
    let progress_invalid = word.iter().any(|&b| b != 0xFF);
    state.read(2 * S::WRITE_SIZE as u32, word).map_err(|_| BootFault)?;
    let first_step_done = !word.contains(&0xFF);
    Ok(progress_invalid || first_step_done)
}

/// The bootloader's whole decision. Call it once per reset, then jump to ACTIVE on [`BootOutcome::Run`].
///
/// `buf` is scratch of one chunk. The four partitions are handles on the same flash; `active` and `dfu` are
/// the image-sized partitions of the geometry (DFU one erase unit longer).
pub fn boot<A, D, S, C>(
    config: &BootConfig,
    active: A,
    dfu: D,
    state: S,
    mut floor: C,
    buf: &mut [u8],
) -> Result<BootOutcome, BootFault>
where
    A: NorFlash + Clone,
    A::Error: EccError,
    D: NorFlash + Clone,
    D::Error: EccError,
    S: NorFlash + Clone,
    S::Error: EccError,
    C: NorFlash,
    C::Error: EccError,
{
    let geometry = config.geometry;
    let buf = &mut buf[..geometry.chunk as usize];
    let floor_now = read_floor(&mut floor, C::WRITE_SIZE, buf).map_err(|_| BootFault)?;
    let expect = Expect {
        product_id: config.product_id,
        geometry_id: geometry.id(),
        image_len: geometry.image_len(),
        floor: floor_now,
    };
    let verify =
        |flash: &mut dyn FnMut(&mut [u8]) -> Result<ImageDescriptor, ImageError>, buf: &mut [u8]| match flash(buf) {
            Err(ImageError::Flash) => Err(BootFault),
            other => Ok(other),
        };
    let mut verify_active = {
        let mut active = active.clone();
        let trust = config.trust;
        move |buf: &mut [u8]| verify_image(&mut active, &trust, &expect, buf)
    };
    let mut verify_dfu = {
        let mut dfu = dfu.clone();
        let trust = config.trust;
        move |buf: &mut [u8]| verify_image(&mut dfu, &trust, &expect, buf)
    };

    let mut loader = BootLoader::new(BootLoaderConfig {
        active: Masked(active),
        dfu: Masked(dfu),
        state: Guarded::new(state.clone()),
    });
    let mut rejected = None;

    // A staged image is verified before the first page moves. Once the swap has started DFU is a mix of both
    // images, so there is nothing to verify until it is done; ACTIVE is verified below either way.
    let staged =
        loader.read_state(buf).map_err(|_| BootFault)? == State::Swap && !swap_started(&mut Masked(state.clone()))?;
    if staged && let Err(reason) = verify(&mut verify_dfu, buf)? {
        rejected = Some(reason);
        let mut word = [0; MAX_WRITE_SIZE];
        BlockingFirmwareState::new(Guarded::new(state.clone()), &mut word[..S::WRITE_SIZE])
            .mark_booted()
            .map_err(|_| BootFault)?;
    }

    loader.prepare_boot(buf).map_err(|_| BootFault)?;
    let mut now = loader.read_state(buf).map_err(|_| BootFault)?;
    let mut verdict = verify(&mut verify_active, buf)?;

    if let (Err(reason), State::Swap) = (verdict, &now) {
        // The image on trial does not verify (forged boot state, rot): revert without running it.
        rejected = Some(reason);
        loader.prepare_boot(buf).map_err(|_| BootFault)?;
        now = loader.read_state(buf).map_err(|_| BootFault)?;
        verdict = verify(&mut verify_active, buf)?;
    }

    let image = match verdict {
        Ok(image) => image,
        Err(reason) => return Ok(BootOutcome::Recovery { reason }),
    };
    let state = match now {
        State::Swap => BootState::Trial,
        State::Revert => BootState::Reverted,
        State::Boot | State::DfuDetach => BootState::Confirmed,
    };
    if state != BootState::Trial && image.security_counter > floor_now {
        match raise_floor(&mut floor, image.security_counter, buf) {
            // A counter beyond the partition cannot be recorded; the image is signed and stays bootable.
            Ok(_) | Err(FloorError::Exhausted) => {}
            Err(FloorError::Flash(_)) => return Err(BootFault),
        }
    }
    Ok(BootOutcome::Run { image, state, rejected })
}

/// The application's handle on embassy-boot's state partition.
pub struct SwapUpdater<'d, S: NorFlash> {
    state: BlockingFirmwareState<'d, Guarded<S>>,
    peek: Masked<S>,
}

impl<'d, S> SwapUpdater<'d, S>
where
    S: NorFlash + Clone,
    S::Error: EccError,
{
    /// `aligned` is a buffer of exactly one write unit of the state partition.
    pub fn new(state: S, aligned: &'d mut [u8]) -> Self {
        Self {
            state: BlockingFirmwareState::new(Guarded::new(state.clone()), aligned),
            peek: Masked(state),
        }
    }
}

impl<S> BootBackend for SwapUpdater<'_, S>
where
    S: NorFlash + Clone,
    S::Error: EccError,
{
    type Error = BootFault;

    fn boot_state(&mut self) -> Result<BootState, BootFault> {
        Ok(match self.state.get_state().map_err(|_| BootFault)? {
            State::Boot | State::DfuDetach => BootState::Confirmed,
            State::Revert => BootState::Reverted,
            State::Swap if swap_started(&mut self.peek)? => BootState::Trial,
            State::Swap => BootState::Staged,
        })
    }

    fn mark_staged(&mut self) -> Result<(), BootFault> {
        self.state.mark_updated().map_err(|_| BootFault)
    }

    fn confirm(&mut self) -> Result<(), BootFault> {
        self.state.mark_booted().map_err(|_| BootFault)
    }
}
