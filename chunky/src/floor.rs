//! The version floor: a counter that only goes up.
//!
//! The floor is the number of write units at the start of the FLOOR partition that have been written with
//! zeros. Raising it writes one unit once; nothing is ever erased. A unit counts as written when more than half
//! of its bits are zero, so one flipped bit in an erased unit cannot raise the floor and lock the device out.
//! A unit torn by a power cut either counts already or is written again (zeros over anything is legal on
//! non-ECC flash; on ECC flash a torn unit faults, reads as zero through [`read_masked`] and counts).
//!
//! Only the bootloader writes it, when it sees a confirmed image with a higher security counter.

use embedded_storage::nor_flash::{NorFlash, ReadNorFlash};

use crate::ecc::{EccError, read_masked};

/// Why the floor could not be raised.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FloorError<E> {
    /// The counter is beyond what the partition can count.
    Exhausted,
    /// The flash failed.
    Flash(E),
}

/// Highest floor the partition can hold, with write units of `unit` bytes.
pub fn floor_capacity<F: ReadNorFlash>(flash: &F, unit: usize) -> u32 {
    (flash.capacity() / unit) as u32
}

/// Reads the floor. `unit` is the write unit of the flash; `buf` holds at least one.
pub fn read_floor<F>(flash: &mut F, unit: usize, buf: &mut [u8]) -> Result<u32, F::Error>
where
    F: ReadNorFlash,
    F::Error: EccError,
{
    let word = &mut buf[..unit];
    let mut floor = 0;
    while floor < floor_capacity(flash, unit) {
        read_masked(flash, floor * unit as u32, word, unit, 0x00)?;
        let zeros: u32 = word.iter().map(|b| b.count_zeros()).sum();
        if zeros * 2 <= unit as u32 * 8 {
            break;
        }
        floor += 1;
    }
    Ok(floor)
}

/// Raises the floor to `to` (never lowers it). Returns the floor afterwards.
pub fn raise_floor<F>(flash: &mut F, to: u32, buf: &mut [u8]) -> Result<u32, FloorError<F::Error>>
where
    F: NorFlash,
    F::Error: EccError,
{
    let unit = F::WRITE_SIZE;
    let mut floor = read_floor(flash, unit, buf).map_err(FloorError::Flash)?;
    if to > floor_capacity(flash, unit) {
        return Err(FloorError::Exhausted);
    }
    let word = &mut buf[..unit];
    while floor < to {
        word.fill(0x00);
        flash.write(floor * unit as u32, word).map_err(FloorError::Flash)?;
        floor += 1;
    }
    Ok(floor)
}
