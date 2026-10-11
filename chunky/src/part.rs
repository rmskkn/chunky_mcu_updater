//! Partitions of one flash device, and a read-only view.
//!
//! Who may write which region is decided by which handle a piece of code is given: the application gets ACTIVE
//! as [`ReadOnly`], so the apply engine cannot write the running image even by mistake.

use core::cell::RefCell;

use embedded_storage::nor_flash::{ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash};

use crate::ecc::EccError;
use crate::geometry::Region;

/// A partition of a shared flash. Offsets are relative to the partition; nothing outside it is reachable.
pub struct Part<'a, F> {
    flash: &'a RefCell<F>,
    offset: u32,
    len: u32,
}

impl<'a, F> Part<'a, F> {
    /// The partition `region` of `flash`.
    pub fn new(flash: &'a RefCell<F>, region: Region) -> Self {
        Self {
            flash,
            offset: region.offset,
            len: region.len,
        }
    }

    /// The first `len` bytes of this partition.
    pub fn prefix(&self, len: u32) -> Self {
        Self {
            flash: self.flash,
            offset: self.offset,
            len: len.min(self.len),
        }
    }

    fn at(&self, offset: u32, len: usize) -> Option<u32> {
        (offset as u64 + len as u64 <= self.len as u64).then_some(self.offset + offset)
    }
}

impl<F> Clone for Part<'_, F> {
    fn clone(&self) -> Self {
        Self {
            flash: self.flash,
            offset: self.offset,
            len: self.len,
        }
    }
}

/// Error of a [`Part`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PartError<E> {
    /// The access leaves the partition.
    OutOfBounds,
    /// Error of the flash underneath.
    Flash(E),
}

impl<E: NorFlashError> NorFlashError for PartError<E> {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            PartError::OutOfBounds => NorFlashErrorKind::OutOfBounds,
            PartError::Flash(e) => e.kind(),
        }
    }
}

impl<E: EccError> EccError for PartError<E> {
    fn is_ecc(&self) -> bool {
        matches!(self, PartError::Flash(e) if e.is_ecc())
    }
}

impl<F: ErrorType> ErrorType for Part<'_, F> {
    type Error = PartError<F::Error>;
}

impl<F: ReadNorFlash> ReadNorFlash for Part<'_, F> {
    const READ_SIZE: usize = F::READ_SIZE;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        let at = self.at(offset, bytes.len()).ok_or(PartError::OutOfBounds)?;
        self.flash.borrow_mut().read(at, bytes).map_err(PartError::Flash)
    }

    fn capacity(&self) -> usize {
        self.len as usize
    }
}

impl<F: NorFlash> NorFlash for Part<'_, F> {
    const WRITE_SIZE: usize = F::WRITE_SIZE;
    const ERASE_SIZE: usize = F::ERASE_SIZE;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        if from > to || to > self.len {
            return Err(PartError::OutOfBounds);
        }
        self.flash
            .borrow_mut()
            .erase(self.offset + from, self.offset + to)
            .map_err(PartError::Flash)
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        let at = self.at(offset, bytes.len()).ok_or(PartError::OutOfBounds)?;
        self.flash.borrow_mut().write(at, bytes).map_err(PartError::Flash)
    }
}

/// A flash that can only be read: it has no `NorFlash` impl, so no write or erase can be called on it.
#[derive(Clone)]
pub struct ReadOnly<F>(F);

impl<F> ReadOnly<F> {
    /// Gives up the ability to write `flash`.
    pub fn new(flash: F) -> Self {
        Self(flash)
    }
}

impl<F: ErrorType> ErrorType for ReadOnly<F> {
    type Error = F::Error;
}

impl<F: ReadNorFlash> ReadNorFlash for ReadOnly<F> {
    const READ_SIZE: usize = F::READ_SIZE;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        self.0.read(offset, bytes)
    }

    fn capacity(&self) -> usize {
        self.0.capacity()
    }
}
