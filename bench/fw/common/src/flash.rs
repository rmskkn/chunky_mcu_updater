//! Flash regions of the contract as `NorFlash`, on one embassy-stm32 blocking flash driver.

use core::cell::RefCell;

use bench_layout::{FLASH_BASE, PAGE, WRITE};
use embassy_stm32::flash::{Blocking, Flash};
use embedded_storage::nor_flash::{ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash};

/// The one flash driver every region shares.
pub type Shared = RefCell<Flash<'static, Blocking>>;

/// Why a flash operation failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// The read hit an uncorrectable ECC error (the NMI reader caught it).
    Ecc,
    /// Outside the region, or not aligned to the write or erase unit.
    Bounds,
    /// The flash controller reported an error.
    Hw(embassy_stm32::flash::Error),
}

impl NorFlashError for Error {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            Error::Bounds => NorFlashErrorKind::OutOfBounds,
            Error::Ecc | Error::Hw(_) => NorFlashErrorKind::Other,
        }
    }
}

impl chunky::ecc::EccError for Error {
    fn is_ecc(&self) -> bool {
        *self == Error::Ecc
    }
}

/// One region (`bench_layout::Region`), offsets from its start.
#[derive(Clone, Copy)]
pub struct Region<'a> {
    flash: &'a Shared,
    region: bench_layout::Region,
}

impl<'a> Region<'a> {
    pub fn new(flash: &'a Shared, region: bench_layout::Region) -> Self {
        Self { flash, region }
    }

    fn check(&self, offset: u32, len: usize) -> Result<u32, Error> {
        let end = offset
            .checked_add(u32::try_from(len).map_err(|_| Error::Bounds)?)
            .ok_or(Error::Bounds)?;
        if end > self.region.len {
            return Err(Error::Bounds);
        }
        Ok(self.region.at + offset)
    }
}

impl ErrorType for Region<'_> {
    type Error = Error;
}

impl ReadNorFlash for Region<'_> {
    const READ_SIZE: usize = 1;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Error> {
        let at = self.check(offset, bytes.len())?;
        if crate::ecc::read(at, bytes) {
            Ok(())
        } else {
            Err(Error::Ecc)
        }
    }

    fn capacity(&self) -> usize {
        self.region.len as usize
    }
}

impl NorFlash for Region<'_> {
    const WRITE_SIZE: usize = WRITE as usize;
    const ERASE_SIZE: usize = PAGE as usize;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), Error> {
        if from > to || !from.is_multiple_of(PAGE) || !to.is_multiple_of(PAGE) {
            return Err(Error::Bounds);
        }
        let at = self.check(from, (to - from) as usize)?;
        let base = at - FLASH_BASE;
        self.flash
            .borrow_mut()
            .blocking_erase(base, base + (to - from))
            .map_err(Error::Hw)
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Error> {
        if !offset.is_multiple_of(WRITE) || !bytes.len().is_multiple_of(WRITE as usize) {
            return Err(Error::Bounds);
        }
        let at = self.check(offset, bytes.len())?;
        self.flash
            .borrow_mut()
            .blocking_write(at - FLASH_BASE, bytes)
            .map_err(Error::Hw)
    }
}
