//! chunky's boot backend on hardware bank swap: dual-bank STM32 parts with a SWAP_BANK option bit (H7, and
//! behind the same [`BankSwitch`] the dual-bank G0, G4 and L4).
//!
//! The flash is two banks. The one mapped first holds the running image (ACTIVE), the other is where an update
//! is staged (DFU). Switching is one option bit and a reset: nothing is copied, and the old image stays where
//! it is until the next update overwrites it, so a revert is the same bit flipped back.
//!
//! - [`boot`] is the bootloader's whole decision. A staged image is verified (seal, product, geometry, version
//!   floor, every byte) before the bit is programmed and again on every boot; a new image gets one trial boot;
//!   one that does not confirm, or stops verifying, is left by flipping the bit back, after the image to go
//!   back to has been verified the same way. The floor is raised once an image is confirmed.
//! - [`BankUpdater`] is the application's [`BootBackend`]: mark staged, confirm, and say where things stand.
//! - What is remembered between resets is a log of write-once flags in the STATE partition, in the `log`
//!   module: every flag is one write unit written once, every read is masked against ECC faults, and the
//!   partition is erased only with nothing in flight and its header overwritten first.
//!
//! The swap bit moves the whole bank, the bootloader's sector included. So the bootloader is in both banks,
//! and [`boot`] programs the bit only when the other bank's copy is byte for byte its own.
//!
//! The partition table of a bank geometry is written with the swap bit clear. [`BankMap`] says where the
//! partitions are addressed right now.
#![no_std]
#![forbid(unsafe_code)]
#![warn(missing_docs)]

mod log;

use chunky::descriptor::{Expect, ImageDescriptor, ImageError, Trust, verify_image};
use chunky::ecc::{EccError, ReadFault, try_read};
use chunky::floor::{FloorError, raise_floor, read_floor};
use chunky::geometry::{FlashGeometry, Region};
use chunky::{BootBackend, BootState};
use embedded_storage::nor_flash::{NorFlash, ReadNorFlash};

use crate::log::{Flag, Log, Record, Target};

/// Largest write unit this crate handles (H7: 32 bytes). The smallest is 8 (G0, G4, L4).
pub const MAX_WRITE_SIZE: usize = 32;

/// The SWAP_BANK option bit of the chip.
pub trait BankSwitch {
    /// Error of the option byte access.
    type Error;

    /// True while the banks are swapped: the bit as the last reset loaded it.
    fn swapped(&mut self) -> Result<bool, Self::Error>;

    /// Programs the bit. It takes effect at the next reset; the caller resets right away.
    fn set_swapped(&mut self, swapped: bool) -> Result<(), Self::Error>;
}

/// Where the partitions of a bank geometry are addressed right now.
///
/// ACTIVE is always the image slot of the bank mapped first and DFU the one of the other bank. STATE and FLOOR
/// stay in the bank they were put in, so their addresses depend on the swap bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BankMap {
    /// The bootloader that is running: the start of the bank mapped first, up to its first partition.
    pub loader: Region,
    /// The other bank's copy of the bootloader.
    pub other_loader: Region,
    /// The running image.
    pub active: Region,
    /// Where an update is staged, and what a trial reverts to.
    pub dfu: Region,
    /// The state log.
    pub state: Region,
    /// The version floor.
    pub floor: Region,
}

impl BankMap {
    /// The map for `geometry` with the swap bit as [`BankSwitch::swapped`] reports it.
    pub const fn new(geometry: &FlashGeometry, swapped: bool) -> Self {
        let bank = geometry.bank_len();
        let parts = [geometry.state, geometry.floor, geometry.active, geometry.dfu];
        let mut loader_len = bank;
        let mut i = 0;
        while i < parts.len() {
            if parts[i].offset % bank < loader_len {
                loader_len = parts[i].offset % bank;
            }
            i += 1;
        }
        let (state, floor) = if swapped {
            (geometry.swapped(geometry.state), geometry.swapped(geometry.floor))
        } else {
            (geometry.state, geometry.floor)
        };
        Self {
            loader: Region {
                offset: 0,
                len: loader_len,
            },
            other_loader: Region {
                offset: bank,
                len: loader_len,
            },
            active: geometry.active,
            dfu: geometry.dfu,
            state,
            floor,
        }
    }
}

/// The flash or the option bytes failed (or, in the simulator, power was cut). Nothing can be decided: reset
/// and try again.
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

/// The bootloader's handles on the flash, addressed as [`BankMap`] says.
pub struct BankParts<A, D, S, C, L> {
    /// The running image.
    pub active: A,
    /// The other bank's image slot.
    pub dfu: D,
    /// The state log.
    pub state: S,
    /// The version floor.
    pub floor: C,
    /// The running bootloader.
    pub loader: L,
    /// The other bank's copy of it.
    pub other_loader: L,
}

/// Why the bootloader did not switch to the other bank.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Rejected {
    /// The image there is refused.
    Image(ImageError),
    /// The other bank does not hold this bootloader: after a switch nothing would boot.
    Loader,
    /// The state log holds flags the bootloader never writes.
    State,
}

/// What the bootloader decided.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootOutcome {
    /// Jump to ACTIVE: it holds this verified image.
    Run {
        /// The image in ACTIVE.
        image: ImageDescriptor,
        /// `Trial` when the image must confirm, `Reverted` after a revert until the next update, else
        /// `Confirmed`.
        state: BootState,
        /// Why the other bank was refused on this boot, if it was: a staged image that is not switched to,
        /// or (with `Trial`) a trial that is over and has nothing verified to go back to.
        rejected: Option<Rejected>,
    },
    /// The swap bit was programmed: reset now. The bootloader of the other bank carries on.
    Reset,
    /// ACTIVE does not verify and there is nothing to switch to. Do not jump; wait for a wired reflash.
    Recovery {
        /// Why ACTIVE is refused.
        reason: ImageError,
    },
}

/// Verifies an image slot. A flash failure is a fault; anything wrong with the image is a verdict.
fn verify<F>(
    flash: &mut F,
    trust: &Trust,
    expect: &Expect,
    buf: &mut [u8],
) -> Result<Result<ImageDescriptor, ImageError>, BootFault>
where
    F: ReadNorFlash,
    F::Error: EccError,
{
    match verify_image(flash, trust, expect, buf) {
        Err(ImageError::Flash) => Err(BootFault),
        verdict => Ok(verdict),
    }
}

/// True when the two bootloader copies are the same bytes.
fn same_loader<L>(own: &mut L, other: &mut L, buf: &mut [u8]) -> Result<bool, BootFault>
where
    L: ReadNorFlash,
    L::Error: EccError,
{
    if own.capacity() != other.capacity() {
        return Ok(false);
    }
    let (a, b) = buf.split_at_mut(buf.len() / 2);
    let b = &mut b[..a.len()];
    let mut offset = 0;
    while offset < own.capacity() {
        let len = a.len().min(own.capacity() - offset);
        for (flash, piece) in [(&mut *own, &mut a[..len]), (&mut *other, &mut b[..len])] {
            match try_read(flash, offset as u32, piece) {
                Ok(()) => {}
                Err(ReadFault::Ecc) => return Ok(false),
                Err(ReadFault::Flash(_)) => return Err(BootFault),
            }
        }
        if a[..len] != b[..len] {
            return Ok(false);
        }
        offset += len;
    }
    Ok(true)
}

/// The bootloader's whole decision. Call it once per reset; reset again on [`BootOutcome::Reset`], jump to
/// ACTIVE on [`BootOutcome::Run`].
///
/// `buf` is scratch of one chunk.
pub fn boot<A, D, S, C, L, W>(
    config: &BootConfig,
    parts: BankParts<A, D, S, C, L>,
    switch: &mut W,
    buf: &mut [u8],
) -> Result<BootOutcome, BootFault>
where
    A: ReadNorFlash,
    A::Error: EccError,
    D: ReadNorFlash,
    D::Error: EccError,
    S: NorFlash,
    S::Error: EccError,
    C: NorFlash,
    C::Error: EccError,
    L: ReadNorFlash,
    L::Error: EccError,
    W: BankSwitch,
{
    let BankParts {
        mut active,
        mut dfu,
        state,
        mut floor,
        mut loader,
        mut other_loader,
    } = parts;
    let geometry = config.geometry;
    let buf = &mut buf[..geometry.chunk as usize];
    let now = switch.swapped().map_err(|_| BootFault)?;
    let floor_now = read_floor(&mut floor, C::WRITE_SIZE, buf).map_err(|_| BootFault)?;
    let expect = Expect {
        product_id: config.product_id,
        geometry_id: geometry.id(),
        image_len: geometry.image_len(),
        floor: floor_now,
    };
    let trust = config.trust;
    // Everything a switch needs of the other bank, forward or back: this bootloader, and an image that passes
    // every check ACTIVE passes on a boot, the floor included.
    let mut other_bank = |buf: &mut [u8]| -> Result<Result<(), Rejected>, BootFault> {
        if let Err(reason) = verify(&mut dfu, &trust, &expect, buf)? {
            return Ok(Err(Rejected::Image(reason)));
        }
        if !same_loader(&mut loader, &mut other_loader, buf)? {
            return Ok(Err(Rejected::Loader));
        }
        Ok(Ok(()))
    };

    let mut log = Log::new(state);
    let mut rejected = None;
    if let Some((index, record)) = log.scan().map_err(|_| BootFault)?.open() {
        let flag = |log: &mut Log<S>, flag| log.set(index, flag).map_err(|_| BootFault);
        match (record.target(), record.tried) {
            // An update is staged and the switch has not happened: not started, or cut short before the bit
            // took its new value. The other bank is verified every time before the bit is programmed.
            (Target::Unset, false) => match other_bank(buf)? {
                Ok(()) => {
                    flag(&mut log, Flag::target(!now))?;
                    switch.set_swapped(!now).map_err(|_| BootFault)?;
                    return Ok(BootOutcome::Reset);
                }
                Err(reason) => {
                    rejected = Some(reason);
                    flag(&mut log, Flag::Closed)?;
                }
            },
            (Target::To(target), false) if target != now => match other_bank(buf)? {
                Ok(()) => {
                    switch.set_swapped(target).map_err(|_| BootFault)?;
                    return Ok(BootOutcome::Reset);
                }
                Err(reason) => {
                    rejected = Some(reason);
                    flag(&mut log, Flag::Closed)?;
                }
            },
            // Back in the old bank after a trial: the revert is done.
            (Target::To(target), true) if target != now => flag(&mut log, Flag::Closed)?,
            // In the new bank.
            (Target::To(_), tried) => {
                let verdict = verify(&mut active, &trust, &expect, buf)?;
                if let (Ok(image), false) = (verdict, tried) {
                    // The one trial boot. It is spent before the image runs, so whatever the image does, the
                    // next reset finds it tried.
                    flag(&mut log, Flag::Tried)?;
                    return Ok(BootOutcome::Run {
                        image,
                        state: BootState::Trial,
                        rejected: None,
                    });
                }
                // The trial is over without a confirmation, or the image on trial does not verify (forged
                // state, rot): back to the other bank, without running it again.
                flag(&mut log, Flag::Tried)?;
                return match (other_bank(buf)?, verdict) {
                    (Ok(()), _) => {
                        switch.set_swapped(!now).map_err(|_| BootFault)?;
                        Ok(BootOutcome::Reset)
                    }
                    // Nothing verified to go back to. A verified image is better than none: it stays on
                    // trial, and the floor stays where it is.
                    (Err(reason), Ok(image)) => Ok(BootOutcome::Run {
                        image,
                        state: BootState::Trial,
                        rejected: Some(reason),
                    }),
                    (Err(_), Err(reason)) => Ok(BootOutcome::Recovery { reason }),
                };
            }
            // Flags the bootloader never writes: a trial without a switch, or a switch to both sides.
            (Target::Unset, true) | (Target::Garbage, _) => {
                rejected = Some(Rejected::State);
                flag(&mut log, Flag::Closed)?;
            }
        }
    }

    // Nothing is in flight. The log gets its header on a new device, and is started over when it is used up,
    // so the application always finds a record for the next update.
    let view = log.scan().map_err(|_| BootFault)?;
    if view.open().is_some() {
        return Err(BootFault);
    }
    if !view.valid || view.free.is_none() {
        log.format().map_err(|_| BootFault)?;
    }
    let state = match view.last {
        Some((_, record)) if record.reverted() => BootState::Reverted,
        _ => BootState::Confirmed,
    };
    let image = match verify(&mut active, &trust, &expect, buf)? {
        Ok(image) => image,
        Err(reason) => return Ok(BootOutcome::Recovery { reason }),
    };
    if image.security_counter > floor_now {
        match raise_floor(&mut floor, image.security_counter, buf) {
            // A counter beyond the partition cannot be recorded; the image is signed and stays bootable.
            Ok(_) | Err(FloorError::Exhausted) => {}
            Err(FloorError::Flash(_)) => return Err(BootFault),
        }
    }
    Ok(BootOutcome::Run { image, state, rejected })
}

/// Why the application's handle failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum UpdaterError {
    /// The flash failed.
    Flash,
    /// The state log has no record to stage into: an update is in flight, or the bootloader has not run
    /// since the log was used up or lost.
    NoRecord,
}

/// The application's handle on the state log. It sets two flags, "staged" and "confirmed", and erases nothing.
pub struct BankUpdater<S> {
    log: Log<S>,
}

impl<S> BankUpdater<S>
where
    S: NorFlash,
    S::Error: EccError,
{
    /// `state` is the STATE partition as addressed right now ([`BankMap`]).
    pub fn new(state: S) -> Self {
        Self { log: Log::new(state) }
    }
}

fn state_of(record: &Record) -> BootState {
    if record.confirmed {
        BootState::Confirmed
    } else if record.closed {
        if record.tried {
            BootState::Reverted
        } else {
            BootState::Confirmed
        }
    } else if record.tried {
        BootState::Trial
    } else {
        BootState::Staged
    }
}

impl<S> BootBackend for BankUpdater<S>
where
    S: NorFlash,
    S::Error: EccError,
{
    type Error = UpdaterError;

    fn boot_state(&mut self) -> Result<BootState, UpdaterError> {
        let view = self.log.scan().map_err(|_| UpdaterError::Flash)?;
        Ok(view.last.map_or(BootState::Confirmed, |(_, record)| state_of(&record)))
    }

    fn mark_staged(&mut self) -> Result<(), UpdaterError> {
        let view = self.log.scan().map_err(|_| UpdaterError::Flash)?;
        match view.open() {
            Some((_, record)) if state_of(&record) == BootState::Staged => Ok(()),
            Some(_) => Err(UpdaterError::NoRecord),
            None => {
                let record = view.free.ok_or(UpdaterError::NoRecord)?;
                self.log.set(record, Flag::Staged).map_err(|_| UpdaterError::Flash)
            }
        }
    }

    fn confirm(&mut self) -> Result<(), UpdaterError> {
        let view = self.log.scan().map_err(|_| UpdaterError::Flash)?;
        match view.open() {
            Some((index, record)) if record.tried => {
                self.log.set(index, Flag::Confirmed).map_err(|_| UpdaterError::Flash)
            }
            _ => Ok(()),
        }
    }
}
