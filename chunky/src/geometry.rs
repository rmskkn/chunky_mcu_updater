//! Flash geometry: the three sizes (chunk, erase, write) and where the partitions sit.
//!
//! [`crate::series`] computes one from the MCU series, the part and the profile the application chose; this
//! module is the result and its rules.
//!
//! A geometry is a `const`, checked at build time: `const GEO: FlashGeometry = FlashGeometry { .. }.checked();`
//! fails the build with a named reason when the layout cannot work, instead of failing in the field.
//!
//! ```
//! use chunky::geometry::*;
//! const GOOD: FlashGeometry = FlashGeometry {
//!     family: Family::Stm32G0,
//!     backend: BackendKind::Swap,
//!     chunk: 1024,
//!     erase: 2048,
//!     write: 8,
//!     state: Region { offset: 0, len: 2048 },
//!     floor: Region { offset: 2048, len: 2048 },
//!     active: Region { offset: 4096, len: 8192 },
//!     dfu: Region { offset: 12288, len: 10240 },
//! }
//! .checked();
//! assert_eq!(GOOD.chunks(), 8);
//! ```
//!
//! The same layout with a chunk that does not divide the erase unit stops the build:
//!
//! ```compile_fail
//! use chunky::geometry::*;
//! const BAD: FlashGeometry = FlashGeometry {
//!     family: Family::Stm32G0,
//!     backend: BackendKind::Swap,
//!     chunk: 768,
//!     erase: 2048,
//!     write: 8,
//!     state: Region { offset: 0, len: 2048 },
//!     floor: Region { offset: 2048, len: 2048 },
//!     active: Region { offset: 4096, len: 8192 },
//!     dfu: Region { offset: 12288, len: 10240 },
//! }
//! .checked();
//! ```

use crate::descriptor::DESCRIPTOR_LEN;

/// Flash family: decides the write rules the layout must respect.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Family {
    /// STM32F1: 16-bit program unit, written once (or to zero), no ECC.
    Stm32F1 = 1,
    /// STM32G0/G4/L4: 64-bit program unit with ECC, written once.
    Stm32G0 = 2,
    /// STM32H7: 256-bit flash word with ECC, written once (H7A3/H7B3/H7B0: 128-bit).
    Stm32H7 = 3,
    /// RP2350/RP2354: QSPI NOR, any 1 to 0 per byte.
    Rp2350 = 4,
}

/// How the device switches between images.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum BackendKind {
    /// embassy-boot: ACTIVE and DFU are swapped page by page.
    Swap = 1,
    /// Hardware bank swap (H7 and other dual-bank parts).
    Bank = 2,
    /// RP2350 boot ROM A/B partitions with try-before-you-buy.
    RpBootrom = 3,
}

/// Write units per record of the bank backend's state log (`chunky_bank`): six flags and two spare.
pub const BANK_RECORD_UNITS: u32 = 8;

/// A span of flash, in bytes from the start of the flash device.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    /// First byte.
    pub offset: u32,
    /// Length in bytes.
    pub len: u32,
}

impl Region {
    /// One past the last byte.
    pub const fn end(&self) -> u64 {
        self.offset as u64 + self.len as u64
    }

    const fn overlaps(&self, other: &Region) -> bool {
        (self.offset as u64) < other.end() && (other.offset as u64) < self.end()
    }
}

/// A run of flash with one erase size (a chip is one or more runs; F4 has three).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct EraseRun {
    /// First byte of the run.
    pub start: u32,
    /// Length of the run in bytes.
    pub len: u32,
    /// Erase unit inside the run.
    pub erase: u32,
}

/// Why a geometry cannot work.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum GeometryError {
    /// A size is zero.
    ZeroSize,
    /// The write unit must divide the chunk.
    WriteNotDividingChunk,
    /// The chunk must divide the erase unit.
    ChunkNotDividingErase,
    /// A chunk must hold the image descriptor.
    ChunkTooSmall,
    /// The write unit must divide the image descriptor.
    WriteNotDividingDescriptor,
    /// The write unit does not fit the family.
    FamilyWriteSize,
    /// A partition does not start or end on an erase unit.
    Misaligned,
    /// A partition is empty.
    EmptyPartition,
    /// Two partitions overlap.
    Overlap,
    /// DFU is smaller than the backend needs (swap: ACTIVE plus one erase unit).
    DfuTooSmall,
    /// STATE cannot hold what the backend keeps there (swap: the progress of ACTIVE; bank: header and a record).
    StateTooSmall,
    /// Bank swap: ACTIVE and DFU are not the same span of two banks, or a partition crosses from one bank
    /// into the other.
    BankLayout,
    /// More chunks than a 16-bit index can address.
    TooManyChunks,
    /// A partition crosses flash with another erase size.
    NonUniformErase,
    /// A partition lies (partly) outside the chip's flash.
    OutsideFlash,
}

/// Chunk, erase and write sizes plus the partition table of one device.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FlashGeometry {
    /// Flash family.
    pub family: Family,
    /// Boot backend.
    pub backend: BackendKind,
    /// Delta and hash unit in bytes.
    pub chunk: u32,
    /// Erase unit in bytes, the same across every partition.
    pub erase: u32,
    /// Program unit in bytes.
    pub write: u32,
    /// Boot state of the backend.
    pub state: Region,
    /// Version floor counter, written only by the bootloader.
    pub floor: Region,
    /// The running image. Read-only for the application.
    pub active: Region,
    /// Where an update is staged.
    pub dfu: Region,
}

impl FlashGeometry {
    /// Chunks in the image (ACTIVE, and the part of DFU an update is staged into).
    pub const fn chunks(&self) -> u32 {
        self.active.len / self.chunk
    }

    /// Erase blocks in the image.
    pub const fn blocks(&self) -> u32 {
        self.active.len / self.erase
    }

    /// Chunks per erase block.
    pub const fn chunks_per_block(&self) -> u32 {
        self.erase / self.chunk
    }

    /// Bytes of the image the SHA-256 covers: everything but the descriptor at the end.
    pub const fn image_len(&self) -> u32 {
        self.active.len - DESCRIPTOR_LEN as u32
    }

    /// Bank backend: the size of one bank. ACTIVE and DFU are the same span of the first and the second bank,
    /// so it is the distance between them.
    pub const fn bank_len(&self) -> u32 {
        self.dfu.offset - self.active.offset
    }

    /// Bank backend: where `region` is addressed while the banks are swapped.
    ///
    /// The partition table says where things are with the swap bit clear. With it set the two banks trade
    /// addresses: ACTIVE and DFU keep their meaning (ACTIVE is always the image in the bank mapped first), and
    /// STATE and FLOOR, which stay in the bank they were put in, are found one bank further or nearer.
    pub const fn swapped(&self, region: Region) -> Region {
        let bank = self.bank_len();
        Region {
            offset: if region.offset < bank {
                region.offset + bank
            } else {
                region.offset - bank
            },
            len: region.len,
        }
    }

    /// Checks the three sizes against each other, the descriptor and the family.
    pub(crate) const fn validate_sizes(
        family: Family,
        chunk: u32,
        erase: u32,
        write: u32,
    ) -> Result<(), GeometryError> {
        if chunk == 0 || erase == 0 || write == 0 {
            return Err(GeometryError::ZeroSize);
        }
        if !chunk.is_multiple_of(write) {
            return Err(GeometryError::WriteNotDividingChunk);
        }
        if !erase.is_multiple_of(chunk) {
            return Err(GeometryError::ChunkNotDividingErase);
        }
        if chunk < DESCRIPTOR_LEN as u32 {
            return Err(GeometryError::ChunkTooSmall);
        }
        if !(DESCRIPTOR_LEN as u32).is_multiple_of(write) {
            return Err(GeometryError::WriteNotDividingDescriptor);
        }
        let family_ok = match family {
            Family::Stm32F1 => write == 2 || write == 4,
            Family::Stm32G0 => write == 8,
            Family::Stm32H7 => write == 16 || write == 32,
            Family::Rp2350 => write == 1,
        };
        if !family_ok {
            return Err(GeometryError::FamilyWriteSize);
        }
        Ok(())
    }

    /// Checks every rule a layout must meet.
    pub const fn validate(&self) -> Result<(), GeometryError> {
        if let Err(e) = Self::validate_sizes(self.family, self.chunk, self.erase, self.write) {
            return Err(e);
        }
        let parts = [self.state, self.floor, self.active, self.dfu];
        let mut i = 0;
        while i < parts.len() {
            if parts[i].len == 0 {
                return Err(GeometryError::EmptyPartition);
            }
            if !parts[i].offset.is_multiple_of(self.erase) || !parts[i].len.is_multiple_of(self.erase) {
                return Err(GeometryError::Misaligned);
            }
            if parts[i].end() > u32::MAX as u64 {
                return Err(GeometryError::OutsideFlash);
            }
            let mut j = i + 1;
            while j < parts.len() {
                if parts[i].overlaps(&parts[j]) {
                    return Err(GeometryError::Overlap);
                }
                j += 1;
            }
            i += 1;
        }
        if self.chunks() > u16::MAX as u32 {
            return Err(GeometryError::TooManyChunks);
        }
        match self.backend {
            BackendKind::Swap => {
                if (self.dfu.len as u64) < self.active.len as u64 + self.erase as u64 {
                    return Err(GeometryError::DfuTooSmall);
                }
                // embassy-boot: a magic word, a validity word and 4 progress words per page.
                let words = 2 + 4 * self.blocks() as u64;
                if words > (self.state.len / self.write) as u64 {
                    return Err(GeometryError::StateTooSmall);
                }
            }
            BackendKind::Bank => {
                // The two image slots are the same span of two banks: the image is linked for one address
                // and runs from whichever bank is mapped first.
                if self.dfu.len != self.active.len || self.dfu.offset <= self.active.offset {
                    return Err(GeometryError::BankLayout);
                }
                let bank = self.bank_len() as u64;
                let mut i = 0;
                while i < parts.len() {
                    let first = parts[i].end() <= bank;
                    let second = parts[i].offset as u64 >= bank && parts[i].end() <= 2 * bank;
                    if !first && !second {
                        return Err(GeometryError::BankLayout);
                    }
                    i += 1;
                }
                if self.active.end() > bank {
                    return Err(GeometryError::BankLayout);
                }
                // The state log: a header record and at least one update.
                if self.state.len / self.write < 2 * BANK_RECORD_UNITS {
                    return Err(GeometryError::StateTooSmall);
                }
            }
            BackendKind::RpBootrom => {
                if self.dfu.len < self.active.len {
                    return Err(GeometryError::DfuTooSmall);
                }
            }
        }
        Ok(())
    }

    /// Checks the layout against the chip's flash: every partition inside it, on flash with this erase size only.
    pub const fn validate_on(&self, runs: &[EraseRun]) -> Result<(), GeometryError> {
        if let Err(e) = self.validate() {
            return Err(e);
        }
        let parts = [self.state, self.floor, self.active, self.dfu];
        let mut i = 0;
        while i < parts.len() {
            let part = parts[i];
            let mut covered: u64 = 0;
            let mut r = 0;
            while r < runs.len() {
                let run = Region {
                    offset: runs[r].start,
                    len: runs[r].len,
                };
                if run.overlaps(&part) {
                    if runs[r].erase != self.erase {
                        return Err(GeometryError::NonUniformErase);
                    }
                    let lo = if run.offset > part.offset {
                        run.offset as u64
                    } else {
                        part.offset as u64
                    };
                    let hi = if run.end() < part.end() { run.end() } else { part.end() };
                    covered += hi - lo;
                }
                r += 1;
            }
            if covered != part.len as u64 {
                return Err(GeometryError::OutsideFlash);
            }
            i += 1;
        }
        Ok(())
    }

    /// Returns the geometry, or stops the build (in a `const`) with the reason.
    pub const fn checked(self) -> Self {
        if let Err(e) = self.validate() {
            fail(e);
        }
        self
    }

    /// Like [`checked`](Self::checked), also against the chip's erase runs.
    pub const fn checked_on(self, runs: &[EraseRun]) -> Self {
        if let Err(e) = self.validate_on(runs) {
            fail(e);
        }
        self
    }

    /// Identifier of this geometry on the wire. Device and server must agree on it, or nothing is planned
    /// (`GEOMETRY_MISMATCH`). FNV-1a over every field.
    pub const fn id(&self) -> u32 {
        let words = [
            self.family as u32,
            self.backend as u32,
            self.chunk,
            self.erase,
            self.write,
            self.state.offset,
            self.state.len,
            self.floor.offset,
            self.floor.len,
            self.active.offset,
            self.active.len,
            self.dfu.offset,
            self.dfu.len,
        ];
        let mut hash: u32 = 0x811c_9dc5;
        let mut i = 0;
        while i < words.len() {
            let bytes = words[i].to_le_bytes();
            let mut b = 0;
            while b < 4 {
                hash ^= bytes[b] as u32;
                hash = hash.wrapping_mul(0x0100_0193);
                b += 1;
            }
            i += 1;
        }
        hash
    }
}

pub(crate) const fn fail(error: GeometryError) -> ! {
    match error {
        GeometryError::ZeroSize => panic!("chunky geometry: chunk, erase and write sizes must not be zero"),
        GeometryError::WriteNotDividingChunk => panic!("chunky geometry: the write unit must divide the chunk"),
        GeometryError::ChunkNotDividingErase => panic!("chunky geometry: the chunk must divide the erase unit"),
        GeometryError::ChunkTooSmall => panic!("chunky geometry: a chunk must hold the 128-byte image descriptor"),
        GeometryError::WriteNotDividingDescriptor => {
            panic!("chunky geometry: the write unit must divide the 128-byte image descriptor")
        }
        GeometryError::FamilyWriteSize => panic!("chunky geometry: this write unit does not exist on this family"),
        GeometryError::Misaligned => panic!("chunky geometry: a partition does not start and end on an erase unit"),
        GeometryError::EmptyPartition => panic!("chunky geometry: a partition is empty"),
        GeometryError::Overlap => panic!("chunky geometry: two partitions overlap"),
        GeometryError::DfuTooSmall => {
            panic!("chunky geometry: DFU is too small (swap backend: ACTIVE plus one erase unit)")
        }
        GeometryError::StateTooSmall => panic!("chunky geometry: STATE is too small for the backend's boot state"),
        GeometryError::BankLayout => {
            panic!(
                "chunky geometry: bank swap needs ACTIVE and DFU at the same place in two banks, no partition across both"
            )
        }
        GeometryError::TooManyChunks => panic!("chunky geometry: more than 65535 chunks; use a bigger chunk"),
        GeometryError::NonUniformErase => {
            panic!("chunky geometry: a partition crosses flash with another erase size (uneven sectors)")
        }
        GeometryError::OutsideFlash => panic!("chunky geometry: a partition lies outside the chip's flash"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn g0() -> FlashGeometry {
        FlashGeometry {
            family: Family::Stm32G0,
            backend: BackendKind::Swap,
            chunk: 2048,
            erase: 2048,
            write: 8,
            state: Region { offset: 0, len: 2048 },
            floor: Region {
                offset: 2048,
                len: 2048,
            },
            active: Region {
                offset: 4096,
                len: 8 * 2048,
            },
            dfu: Region {
                offset: 4096 + 8 * 2048,
                len: 9 * 2048,
            },
        }
    }

    const GOOD: FlashGeometry = g0().checked();

    #[test]
    fn good_geometry_passes() {
        assert_eq!(GOOD.chunks(), 8);
        assert_eq!(GOOD.validate(), Ok(()));
    }

    #[test]
    fn every_rule_has_a_named_failure() {
        type Change = fn(&mut FlashGeometry);
        let cases: [(Change, GeometryError); 11] = [
            (|g| g.chunk = 0, GeometryError::ZeroSize),
            (|g| g.chunk = 2044, GeometryError::WriteNotDividingChunk),
            (|g| g.chunk = 768, GeometryError::ChunkNotDividingErase),
            (|g| g.chunk = 64, GeometryError::ChunkTooSmall),
            (|g| g.write = 4, GeometryError::FamilyWriteSize),
            (|g| g.active.offset += 8, GeometryError::Misaligned),
            (|g| g.floor.len = 0, GeometryError::EmptyPartition),
            (|g| g.floor.offset = 0, GeometryError::Overlap),
            (|g| g.dfu.len = 8 * 2048, GeometryError::DfuTooSmall),
            (
                |g| {
                    g.active.len = 64 * 2048;
                    g.dfu = Region {
                        offset: 4096 + 64 * 2048,
                        len: 65 * 2048,
                    };
                },
                GeometryError::StateTooSmall,
            ),
            (
                |g| {
                    g.family = Family::Stm32H7;
                    g.write = 32;
                    g.chunk = 2048;
                    g.erase = 131_072;
                },
                GeometryError::Misaligned,
            ),
        ];
        for (change, expected) in cases {
            let mut geometry = g0();
            change(&mut geometry);
            assert_eq!(geometry.validate(), Err(expected));
        }
    }

    /// Two banks of eight 8 KB sectors: loader, six image sectors, then STATE (first bank) or FLOOR (second).
    const fn banks() -> FlashGeometry {
        const S: u32 = 8192;
        FlashGeometry {
            family: Family::Stm32H7,
            backend: BackendKind::Bank,
            chunk: 2048,
            erase: S,
            write: 32,
            state: Region { offset: 7 * S, len: S },
            floor: Region { offset: 15 * S, len: S },
            active: Region { offset: S, len: 6 * S },
            dfu: Region {
                offset: 9 * S,
                len: 6 * S,
            },
        }
    }

    #[test]
    fn bank_layout_rules() {
        const GOOD: FlashGeometry = banks().checked();
        assert_eq!(GOOD.bank_len(), 8 * 8192);
        // Swapped, STATE is found in the second bank and FLOOR in the first.
        assert_eq!(GOOD.swapped(GOOD.state).offset, 15 * 8192);
        assert_eq!(GOOD.swapped(GOOD.floor).offset, 7 * 8192);
        assert_eq!(GOOD.swapped(GOOD.active), GOOD.dfu);
        assert_eq!(GOOD.swapped(GOOD.dfu), GOOD.active);

        type Change = fn(&mut FlashGeometry);
        let cases: [(Change, GeometryError); 5] = [
            // The slots differ in size.
            (|g| g.dfu.len -= 8192, GeometryError::BankLayout),
            // DFU before ACTIVE: no second bank.
            (
                |g| core::mem::swap(&mut g.active, &mut g.dfu),
                GeometryError::BankLayout,
            ),
            // STATE beyond the second bank.
            (|g| g.state.offset = 16 * 8192, GeometryError::BankLayout),
            // ACTIVE and DFU so close that ACTIVE runs into the second bank.
            (
                |g| {
                    g.dfu.offset = 7 * 8192;
                    g.state.offset = 13 * 8192;
                    g.floor.offset = 0;
                },
                GeometryError::BankLayout,
            ),
            // A state partition of one erase unit of 256 bytes cannot hold a header and a record.
            (
                |g| {
                    g.erase = 256;
                    g.chunk = 256;
                    g.state.len = 256;
                },
                GeometryError::StateTooSmall,
            ),
        ];
        for (change, expected) in cases {
            let mut geometry = banks();
            change(&mut geometry);
            assert_eq!(geometry.validate(), Err(expected));
        }
    }

    #[test]
    fn uneven_sectors_are_refused() {
        // F4-like: small sectors first, then a big one.
        let runs = [
            EraseRun {
                start: 0,
                len: 4 * 2048,
                erase: 2048,
            },
            EraseRun {
                start: 4 * 2048,
                len: 1 << 20,
                erase: 16_384,
            },
        ];
        assert_eq!(GOOD.validate_on(&runs), Err(GeometryError::NonUniformErase));
        let uniform = [EraseRun {
            start: 0,
            len: 19 * 2048,
            erase: 2048,
        }];
        assert_eq!(GOOD.validate_on(&uniform), Ok(()));
        let short = [EraseRun {
            start: 0,
            len: 18 * 2048,
            erase: 2048,
        }];
        assert_eq!(GOOD.validate_on(&short), Err(GeometryError::OutsideFlash));
    }

    #[test]
    fn id_changes_with_any_field() {
        let mut other = g0();
        other.dfu.len += 2048;
        assert_ne!(GOOD.id(), other.id());
        assert_eq!(GOOD.id(), g0().id());
    }
}
