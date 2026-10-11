//! A simulated NOR flash with the write rules of one family.
//!
//! | Model | Program unit | Rule | ECC | Erase |
//! |---|---|---|---|---|
//! | [`F1`] | 16 bit | only an erased half-word, or the value `0x0000` | no | 1 KB |
//! | [`G0`] | 64 bit | only an erased double word, or all zeros (which also clears a torn word) | yes | 2 KB |
//! | [`H7`] | 256 bit | only an erased flash word; zeros over a used one are taken but leave it faulting | yes | 128 KB |
//! | [`H7Mini`] | as H7 | as H7 | yes | 8 KB (H7 scaled down, for sweeps) |
//! | [`Rp2350`] | byte | bits only go from 1 to 0 | no | 4 KB |
//!
//! Breaking a rule panics: on hardware it is a programming error flag or silent corruption, and no code path of
//! ours may ever get there.
//!
//! A power cut can be armed at any write or erase. The operation is then torn (some units done, one left
//! half-way, the rest untouched) and the flash is dead until [`SimFlash::power_on`]. A torn unit on an ECC part
//! faults on read ([`SimError::Ecc`]).
//!
//! A flash made with [`SimFlash::with_banks`] is two banks and a SWAP_BANK option bit, as on a dual-bank H7.
//! Every offset is an address as the CPU sees it: with the bit in effect the second bank answers at offset 0
//! and the first one bank further. The bit is programmed with [`SimFlash::program_swap`] (an operation like
//! any other: it is counted and a power cut can hit it, leaving the old or the new value) and takes effect at
//! the next [`SimFlash::reset`], as option bytes do.

use std::marker::PhantomData;

use chunky::EccError;
use chunky::geometry::Family;
use embedded_storage::nor_flash::{ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash};

/// How a program unit may be written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Rule {
    /// Erased, or to zero. No ECC.
    OnceOrZero,
    /// Erased, or to zero. ECC: a torn unit faults.
    EccOnceOrZero,
    /// Erased only. ECC: a torn unit faults.
    EccOnce,
    /// Bits go from 1 to 0, any number of times. No ECC.
    BitsToZero,
}

impl Rule {
    fn ecc(self) -> bool {
        matches!(self, Rule::EccOnceOrZero | Rule::EccOnce)
    }
}

/// A flash family as the simulator models it.
pub trait FlashModel: 'static {
    /// Name in panics.
    const NAME: &'static str;
    /// Family for the geometry.
    const FAMILY: Family;
    /// `NorFlash::WRITE_SIZE` of the real driver.
    const WRITE_SIZE: usize;
    /// Erase unit.
    const ERASE_SIZE: usize;
    /// Unit the write rule applies to.
    const UNIT: usize;
    /// The write rule.
    const RULE: Rule;
}

macro_rules! model {
    ($(#[$doc:meta])* $name:ident, $family:expr, write $write:expr, erase $erase:expr, unit $unit:expr, $rule:expr) => {
        $(#[$doc])*
        #[derive(Clone, Copy, Debug)]
        pub struct $name;
        impl FlashModel for $name {
            const NAME: &'static str = stringify!($name);
            const FAMILY: Family = $family;
            const WRITE_SIZE: usize = $write;
            const ERASE_SIZE: usize = $erase;
            const UNIT: usize = $unit;
            const RULE: Rule = $rule;
        }
    };
}

model!(
    /// STM32F103: half-words written once or to zero, 1 KB pages.
    F1, Family::Stm32F1, write 4, erase 1024, unit 2, Rule::OnceOrZero
);
model!(
    /// STM32G0: 64-bit double words with ECC, 2 KB pages.
    G0, Family::Stm32G0, write 8, erase 2048, unit 8, Rule::EccOnceOrZero
);
model!(
    /// STM32H743: 256-bit flash words with ECC, 128 KB sectors.
    H7, Family::Stm32H7, write 32, erase 131_072, unit 32, Rule::EccOnce
);
model!(
    /// H7 write rules with 8 KB sectors: small enough to cut power at every operation.
    H7Mini, Family::Stm32H7, write 32, erase 8192, unit 32, Rule::EccOnce
);
model!(
    /// RP2350 QSPI NOR: bytes, bits to zero, 4 KB sectors.
    Rp2350, Family::Rp2350, write 1, erase 4096, unit 1, Rule::BitsToZero
);

/// Error of a [`SimFlash`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SimError {
    /// Power is off (it was cut during this or an earlier operation).
    PowerCut,
    /// The read hit a unit with an uncorrectable ECC error.
    Ecc,
    /// The access leaves the flash.
    OutOfBounds,
    /// Offset or length is not a multiple of the write or erase size.
    NotAligned,
}

impl NorFlashError for SimError {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            SimError::OutOfBounds => NorFlashErrorKind::OutOfBounds,
            SimError::NotAligned => NorFlashErrorKind::NotAligned,
            SimError::PowerCut | SimError::Ecc => NorFlashErrorKind::Other,
        }
    }
}

impl EccError for SimError {
    fn is_ecc(&self) -> bool {
        *self == SimError::Ecc
    }
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        // splitmix64
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        z ^ (z >> 31)
    }

    fn below(&mut self, n: usize) -> usize {
        (self.next() % n.max(1) as u64) as usize
    }
}

/// The simulated flash.
pub struct SimFlash<M> {
    mem: Vec<u8>,
    /// Per rule unit: reads fault on ECC.
    bad: Vec<bool>,
    ops: u64,
    cut_at: Option<u64>,
    dead: bool,
    rng: Rng,
    erases: Vec<u32>,
    /// Size of one bank; 0 for a flash without bank swap.
    bank: usize,
    /// The SWAP_BANK option bit as programmed.
    swap_programmed: bool,
    /// The SWAP_BANK bit in effect: the programmed one as it was at the last reset.
    swap_now: bool,
    model: PhantomData<M>,
}

impl<M: FlashModel> SimFlash<M> {
    /// An erased flash of `size` bytes.
    pub fn new(size: usize) -> Self {
        assert_eq!(size % M::ERASE_SIZE, 0);
        Self {
            mem: vec![0xFF; size],
            bad: vec![false; size / M::UNIT],
            ops: 0,
            cut_at: None,
            dead: false,
            rng: Rng(0),
            erases: vec![0; size / M::ERASE_SIZE],
            bank: 0,
            swap_programmed: false,
            swap_now: false,
            model: PhantomData,
        }
    }

    /// An erased flash of two banks of `bank` bytes with a SWAP_BANK option bit, clear.
    pub fn with_banks(bank: usize) -> Self {
        let mut flash = Self::new(2 * bank);
        flash.bank = bank;
        flash
    }

    /// True while the banks are swapped: the bit as the last reset found it.
    pub fn swapped(&self) -> bool {
        self.swap_now
    }

    /// The SWAP_BANK bit as programmed; it takes effect at the next reset.
    pub fn swap_programmed(&self) -> bool {
        self.swap_programmed
    }

    /// Programs the SWAP_BANK option bit. Counted like a write; a power cut during it leaves the old or the
    /// new value. That it never leaves anything else is this model's assumption about option byte programming,
    /// to be confirmed on hardware (HW-2).
    pub fn program_swap(&mut self, swapped: bool) -> Result<(), SimError> {
        assert!(self.bank != 0, "{}: this flash has no bank swap", M::NAME);
        if self.start_op()? {
            if self.rng.below(2) == 1 {
                self.swap_programmed = swapped;
            }
            self.dead = true;
            return Err(SimError::PowerCut);
        }
        self.swap_programmed = swapped;
        Ok(())
    }

    /// A system reset: the option bytes are loaded, so a programmed SWAP_BANK bit takes effect.
    pub fn reset(&mut self) {
        self.swap_now = self.swap_programmed;
    }

    /// Where `len` bytes at CPU address `offset` are in the memory array.
    fn physical(&self, offset: usize, len: usize) -> usize {
        if self.bank == 0 {
            return offset;
        }
        assert!(
            offset / self.bank == (offset + len.max(1) - 1) / self.bank,
            "{}: access at {offset:#x} of {len} bytes crosses from one bank into the other",
            M::NAME
        );
        match (self.swap_now, offset < self.bank) {
            (false, _) => offset,
            (true, true) => offset + self.bank,
            (true, false) => offset - self.bank,
        }
    }

    /// Writes and erases so far, torn ones included.
    pub fn ops(&self) -> u64 {
        self.ops
    }

    /// Cuts power during the write or erase that is `after` operations from now (0: the next one). `seed`
    /// decides how the operation is torn.
    pub fn cut_after(&mut self, after: u64, seed: u64) {
        self.cut_at = Some(self.ops + after);
        self.rng = Rng(seed);
    }

    /// True once a power cut has hit.
    pub fn is_dead(&self) -> bool {
        self.dead
    }

    /// Power is back, which is also a reset. An armed cut that has not hit stays armed.
    pub fn power_on(&mut self) {
        self.dead = false;
        self.reset();
    }

    /// Disarms a cut that has not hit.
    pub fn disarm(&mut self) {
        self.cut_at = None;
    }

    /// Puts `data` at `offset` the way a programmer on the debug port would: no rules, no faults left.
    pub fn load(&mut self, offset: usize, data: &[u8]) {
        let offset = self.physical(offset, data.len());
        self.mem[offset..offset + data.len()].copy_from_slice(data);
        for unit in offset / M::UNIT..(offset + data.len()).div_ceil(M::UNIT) {
            self.bad[unit] = false;
        }
    }

    /// The bytes at `offset`, whatever the ECC state.
    pub fn dump(&self, offset: usize, len: usize) -> Vec<u8> {
        let offset = self.physical(offset, len);
        self.mem[offset..offset + len].to_vec()
    }

    /// Makes the unit at `offset` fault on read, as bit rot beyond what ECC corrects would. ECC models only.
    pub fn inject_ecc(&mut self, offset: usize) {
        assert!(M::RULE.ecc(), "{} has no ECC", M::NAME);
        let offset = self.physical(offset, 1);
        self.bad[offset / M::UNIT] = true;
    }

    /// How often the erase block at `offset` was erased.
    pub fn erase_count(&self, offset: usize) -> u32 {
        self.erases[self.physical(offset, 1) / M::ERASE_SIZE]
    }

    /// The most often erased block's count.
    pub fn max_erase_count(&self) -> u32 {
        self.erases.iter().copied().max().unwrap_or(0)
    }

    fn start_op(&mut self) -> Result<bool, SimError> {
        if self.dead {
            return Err(SimError::PowerCut);
        }
        let cut = self.cut_at == Some(self.ops);
        self.ops += 1;
        if cut {
            self.cut_at = None;
        }
        Ok(cut)
    }

    fn program_unit(&mut self, at: usize, new: &[u8]) {
        let unit = at / M::UNIT;
        let current = &mut self.mem[at..at + M::UNIT];
        let erased = current.iter().all(|&b| b == 0xFF);
        let to_zero = new.iter().all(|&b| b == 0x00);
        let clean = erased && !self.bad[unit];
        match M::RULE {
            Rule::OnceOrZero | Rule::EccOnceOrZero => {
                assert!(
                    clean || to_zero,
                    "{}: program at {at:#x} of a unit that is not erased ({current:02x?} <- {new:02x?})",
                    M::NAME
                );
                // All zeros is the one value that can go over anything: data and ECC bits only ever clear.
                current.copy_from_slice(new);
                self.bad[unit] = false;
            }
            Rule::EccOnce => {
                assert!(
                    clean || to_zero,
                    "{}: program at {at:#x} of a flash word that is not erased ({current:02x?} <- {new:02x?})",
                    M::NAME
                );
                // H7 does not refuse a second program. Zeros over zeros change nothing; zeros over anything
                // else clear the data and leave the ECC bits in doubt, so the word faults until erased.
                let already_zero = current.iter().all(|&b| b == 0x00) && !self.bad[unit];
                if !clean && !already_zero {
                    self.bad[unit] = true;
                }
                current.copy_from_slice(new);
            }
            Rule::BitsToZero => {
                for (cell, &byte) in current.iter_mut().zip(new) {
                    assert!(
                        *cell & byte == byte,
                        "{}: program at {at:#x} would need a bit to go from 0 to 1",
                        M::NAME
                    );
                    *cell = byte;
                }
            }
        }
    }

    /// A program of one unit that lost power half-way.
    fn tear_unit(&mut self, at: usize, new: &[u8]) {
        let unit = at / M::UNIT;
        let partial = |mem: &mut [u8], rng: &mut Rng| {
            for (cell, &byte) in mem.iter_mut().zip(new) {
                *cell &= byte | rng.next() as u8;
            }
        };
        match (M::RULE, self.rng.below(3)) {
            // Power went just before the unit.
            (_, 0) => {}
            // Some bits made it.
            (Rule::OnceOrZero | Rule::BitsToZero, _) | (Rule::EccOnceOrZero, 1) => {
                partial(&mut self.mem[at..at + M::UNIT], &mut self.rng)
            }
            // Data and ECC bits disagree: the unit faults until it is erased.
            (Rule::EccOnceOrZero | Rule::EccOnce, _) => {
                partial(&mut self.mem[at..at + M::UNIT], &mut self.rng);
                self.bad[unit] = true;
            }
        }
    }

    /// An erase of one block that lost power half-way: every unit ends up somewhere between old and erased.
    fn tear_erase(&mut self, block: usize) {
        for at in (block..block + M::ERASE_SIZE).step_by(M::UNIT) {
            let unit = at / M::UNIT;
            match self.rng.below(4) {
                0 => {}
                1 => {
                    self.mem[at..at + M::UNIT].fill(0xFF);
                    self.bad[unit] = false;
                }
                _ if M::RULE.ecc() => self.bad[unit] = true,
                _ => {
                    for cell in &mut self.mem[at..at + M::UNIT] {
                        *cell |= self.rng.next() as u8;
                    }
                }
            }
        }
    }
}

impl<M: FlashModel> ErrorType for SimFlash<M> {
    type Error = SimError;
}

impl<M: FlashModel> ReadNorFlash for SimFlash<M> {
    const READ_SIZE: usize = 1;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), SimError> {
        if self.dead {
            return Err(SimError::PowerCut);
        }
        let offset = offset as usize;
        let end = offset.checked_add(bytes.len()).filter(|&end| end <= self.mem.len());
        end.ok_or(SimError::OutOfBounds)?;
        if bytes.is_empty() {
            return Ok(());
        }
        let offset = self.physical(offset, bytes.len());
        let end = offset + bytes.len();
        if self.bad[offset / M::UNIT..end.div_ceil(M::UNIT)].iter().any(|&bad| bad) {
            return Err(SimError::Ecc);
        }
        bytes.copy_from_slice(&self.mem[offset..end]);
        Ok(())
    }

    fn capacity(&self) -> usize {
        self.mem.len()
    }
}

impl<M: FlashModel> NorFlash for SimFlash<M> {
    const WRITE_SIZE: usize = M::WRITE_SIZE;
    const ERASE_SIZE: usize = M::ERASE_SIZE;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), SimError> {
        let (from, to) = (from as usize, to as usize);
        if from > to || to > self.mem.len() {
            return Err(SimError::OutOfBounds);
        }
        if !from.is_multiple_of(M::ERASE_SIZE) || !to.is_multiple_of(M::ERASE_SIZE) {
            return Err(SimError::NotAligned);
        }
        let cut = self.start_op()?;
        let blocks = (to - from) / M::ERASE_SIZE;
        let from = self.physical(from, to - from);
        let torn = cut.then(|| self.rng.below(blocks));
        for index in 0..blocks {
            let block = from + index * M::ERASE_SIZE;
            if torn.is_some_and(|torn| index >= torn) {
                self.tear_erase(block);
                break;
            }
            self.mem[block..block + M::ERASE_SIZE].fill(0xFF);
            self.bad[block / M::UNIT..(block + M::ERASE_SIZE) / M::UNIT].fill(false);
            self.erases[block / M::ERASE_SIZE] += 1;
        }
        if cut {
            self.dead = true;
            return Err(SimError::PowerCut);
        }
        Ok(())
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), SimError> {
        let offset = offset as usize;
        if offset.checked_add(bytes.len()).is_none_or(|end| end > self.mem.len()) {
            return Err(SimError::OutOfBounds);
        }
        if !offset.is_multiple_of(M::WRITE_SIZE) || !bytes.len().is_multiple_of(M::WRITE_SIZE) {
            return Err(SimError::NotAligned);
        }
        let cut = self.start_op()?;
        let units = bytes.len() / M::UNIT;
        let offset = self.physical(offset, bytes.len());
        let torn = cut.then(|| self.rng.below(units));
        for (index, new) in bytes.chunks(M::UNIT).enumerate() {
            let at = offset + index * M::UNIT;
            if torn.is_some_and(|torn| index >= torn) {
                self.tear_unit(at, new);
                break;
            }
            self.program_unit(at, new);
        }
        if cut {
            self.dead = true;
            return Err(SimError::PowerCut);
        }
        Ok(())
    }
}
