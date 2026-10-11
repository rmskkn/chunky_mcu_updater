//! Reads that survive ECC faults.
//!
//! On ECC flash (G0, G4, L4, H7) a write cut by power loss leaves a word that faults when read: NMI on G0,
//! BusFault on H7. A flash driver for such a part catches the fault and returns an error for which
//! [`EccError::is_ecc`] is true. Code that hashes or boots then treats the data as garbage instead of looping in
//! the fault handler.

use embedded_storage::nor_flash::{ErrorType, NorFlash, ReadNorFlash};

/// A flash error that can say "this read hit an uncorrectable ECC error".
pub trait EccError {
    /// True when the read faulted on ECC; the data is unknown, the flash itself still works.
    fn is_ecc(&self) -> bool;
}

/// Why a [`try_read`] failed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReadFault<E> {
    /// Uncorrectable ECC error somewhere in the range.
    Ecc,
    /// Any other flash error.
    Flash(E),
}

/// Reads `buf.len()` bytes, telling an ECC fault apart from other errors.
pub fn try_read<F>(flash: &mut F, offset: u32, buf: &mut [u8]) -> Result<(), ReadFault<F::Error>>
where
    F: ReadNorFlash,
    F::Error: EccError,
{
    flash.read(offset, buf).map_err(|e| {
        if e.is_ecc() {
            ReadFault::Ecc
        } else {
            ReadFault::Flash(e)
        }
    })
}

/// Reads `buf.len()` bytes; units of `unit` bytes that fault on ECC read as `fill`. Returns whether any did.
pub fn read_masked<F>(flash: &mut F, offset: u32, buf: &mut [u8], unit: usize, fill: u8) -> Result<bool, F::Error>
where
    F: ReadNorFlash,
    F::Error: EccError,
{
    match flash.read(offset, buf) {
        Ok(()) => return Ok(false),
        Err(e) if !e.is_ecc() => return Err(e),
        Err(_) => {}
    }
    let unit = unit.max(1);
    let mut done = 0;
    while done < buf.len() {
        let at = offset as usize + done;
        let len = (unit - at % unit).min(buf.len() - done);
        let piece = &mut buf[done..done + len];
        match flash.read(at as u32, piece) {
            Ok(()) => {}
            Err(e) if e.is_ecc() => piece.fill(fill),
            Err(e) => return Err(e),
        }
        done += len;
    }
    Ok(true)
}

/// A flash whose reads never fail on ECC: faulty write units read as zero.
///
/// Zero is "written" for every write-once flag (embassy-boot's progress words, the floor counter) and is no
/// valid magic, so a torn flag is taken as set and a torn magic as absent. Image data read through it simply
/// fails its hash.
#[derive(Clone)]
pub struct Masked<F>(pub F);

impl<F: ErrorType> ErrorType for Masked<F> {
    type Error = F::Error;
}

impl<F> ReadNorFlash for Masked<F>
where
    F: NorFlash,
    F::Error: EccError,
{
    const READ_SIZE: usize = F::READ_SIZE;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        read_masked(&mut self.0, offset, bytes, F::WRITE_SIZE, 0x00).map(|_| ())
    }

    fn capacity(&self) -> usize {
        self.0.capacity()
    }
}

impl<F> NorFlash for Masked<F>
where
    F: NorFlash,
    F::Error: EccError,
{
    const WRITE_SIZE: usize = F::WRITE_SIZE;
    const ERASE_SIZE: usize = F::ERASE_SIZE;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        self.0.erase(from, to)
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        self.0.write(offset, bytes)
    }
}
