//! A tiny NOR flash for reproducing power-cut bugs in embassy-boot, shared by three partitions.
//!
//! Geometry (STM32F1-like, scaled down): write unit 4 bytes, erase page 64 bytes, ACTIVE 3 pages, DFU 4 pages,
//! STATE 1 page (16 words; embassy-boot uses 2 + 4*3 = 14 of them).
//!
//! Physics kept: a program can only clear bits (AND); an erase can only set bits (to 0xFF). Tearing is
//! word-granular for [`Tear::Words`] (each word of the erased page is either untouched or fully erased) and
//! bit-granular for [`Tear::Bits`] (untouched, erased, or some of the zero bits raised to one, never the other
//! way). [`Tear::Ecc`] is the ECC-flash case: the first `erased` words are erased, the next one is in flight
//! and faults on every read until erased, the rest are untouched.
//!
//! Not modelled: a torn *program* of more than one write unit (embassy-boot never relies on that), erase
//! order across several pages (the state partition here is one page).

pub mod classify;

pub use classify::{Active, Class, PageOf};
use embedded_storage::nor_flash::{ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash};
use std::cell::RefCell;
use std::rc::Rc;

/// Write unit in bytes.
pub const WRITE: usize = 4;
/// Erase page in bytes.
pub const ERASE: usize = 64;
/// ACTIVE pages.
pub const N: usize = 3;
/// Offset of ACTIVE.
pub const ACTIVE_AT: usize = 0;
/// Offset of DFU (N + 1 pages).
pub const DFU_AT: usize = N * ERASE;
/// Offset of STATE (one page).
pub const STATE_AT: usize = DFU_AT + (N + 1) * ERASE;
/// Total size.
pub const TOTAL: usize = STATE_AT + ERASE;
/// Words of STATE that embassy-boot uses (magic, validity, 2N swap, 2N revert).
pub const STATE_WORDS_USED: usize = 2 + 4 * N;

/// How a power cut tears an erase of the STATE page.
#[derive(Clone, Copy, Debug)]
pub enum Tear {
    /// Bit i set: word i of the page is erased, the others untouched.
    Words(u32),
    /// Pseudo-random per word: untouched, erased, or some bits raised.
    Bits(u64),
    /// ECC flash: words `0..erased` erased, word `erased` unreadable, the rest untouched.
    Ecc { erased: usize },
}

/// When power is cut.
#[derive(Clone, Copy, Debug)]
pub enum Cut {
    /// Before the n-th mutating operation (erase or program) after arming; it has no effect.
    BeforeOp(usize),
    /// During the n-th erase of the STATE page after arming.
    StateErase { nth: usize, tear: Tear },
    /// During the n-th mutating operation, which must be a program: only the bytes in `bytes` (bit mask over the
    /// word's bytes) reach the cells.
    TornProgram { nth: usize, bytes: u8 },
}

/// A program of a word that was not erased (the "written twice" question).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Reprogram {
    /// Offset in the whole flash.
    pub at: usize,
    /// The word was all zeros afterwards, i.e. the data written was zeros.
    pub zeros: bool,
}

/// Flash error.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlashError {
    /// Power was cut; nothing works until [`Flash::reboot`].
    PowerLoss,
    /// ECC fault on read.
    Ecc,
    /// Misaligned or out of range.
    Bounds,
}

impl NorFlashError for FlashError {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            FlashError::Bounds => NorFlashErrorKind::OutOfBounds,
            _ => NorFlashErrorKind::Other,
        }
    }
}

/// The flash array.
pub struct Flash {
    /// Contents.
    pub mem: Vec<u8>,
    faulty: Vec<bool>,
    dead: bool,
    cut: Option<Cut>,
    ops: usize,
    state_erases: usize,
    /// Programs of non-erased words since creation.
    pub reprograms: Vec<Reprogram>,
    /// Set when an armed cut has fired.
    pub fired: bool,
}

/// Shared handle.
pub type Shared = Rc<RefCell<Flash>>;

impl Flash {
    /// Fully erased flash.
    pub fn new() -> Shared {
        Rc::new(RefCell::new(Flash {
            mem: vec![0xFF; TOTAL],
            faulty: vec![false; TOTAL / WRITE],
            dead: false,
            cut: None,
            ops: 0,
            state_erases: 0,
            reprograms: vec![],
            fired: false,
        }))
    }

    /// Arms a power cut; operation counters restart.
    pub fn arm(&mut self, cut: Cut) {
        self.cut = Some(cut);
        self.ops = 0;
        self.state_erases = 0;
        self.fired = false;
    }

    /// Power comes back: nothing is armed, the controller works again, torn cells stay torn.
    pub fn reboot(&mut self) {
        self.dead = false;
        self.cut = None;
    }

    /// Number of mutating operations since arming.
    pub fn ops(&self) -> usize {
        self.ops
    }

    fn check(&self, off: usize, len: usize, unit: usize) -> Result<(), FlashError> {
        if !off.is_multiple_of(unit) || !len.is_multiple_of(unit) || off + len > TOTAL {
            return Err(FlashError::Bounds);
        }
        Ok(())
    }

    fn die(&mut self) -> FlashError {
        self.dead = true;
        self.fired = true;
        FlashError::PowerLoss
    }

    fn read(&self, off: usize, buf: &mut [u8]) -> Result<(), FlashError> {
        if self.dead {
            return Err(FlashError::PowerLoss);
        }
        self.check(off, buf.len(), 1)?;
        for w in off / WRITE..(off + buf.len()).div_ceil(WRITE) {
            if self.faulty[w] {
                return Err(FlashError::Ecc);
            }
        }
        buf.copy_from_slice(&self.mem[off..off + buf.len()]);
        Ok(())
    }

    fn program(&mut self, off: usize, data: &[u8]) -> Result<(), FlashError> {
        if self.dead {
            return Err(FlashError::PowerLoss);
        }
        self.check(off, data.len(), WRITE)?;
        let n = self.ops;
        self.ops += 1;
        let mut keep = 0xFFu8;
        match self.cut {
            Some(Cut::BeforeOp(k)) if k == n => return Err(self.die()),
            Some(Cut::TornProgram { nth, bytes }) if nth == n => keep = bytes,
            _ => {}
        }
        for (i, chunk) in data.chunks(WRITE).enumerate() {
            let w = off / WRITE + i;
            if self.mem[w * WRITE..(w + 1) * WRITE].iter().any(|&b| b != 0xFF) {
                let zeros = chunk.iter().all(|&b| b == 0);
                self.reprograms.push(Reprogram { at: w * WRITE, zeros });
            }
            for (j, &b) in chunk.iter().enumerate() {
                if keep == 0xFF || keep & (1 << j) != 0 {
                    self.mem[w * WRITE + j] &= b;
                }
            }
        }
        if keep != 0xFF {
            return Err(self.die());
        }
        Ok(())
    }

    fn erase(&mut self, from: usize, to: usize) -> Result<(), FlashError> {
        if self.dead {
            return Err(FlashError::PowerLoss);
        }
        self.check(from, to.wrapping_sub(from), ERASE)?;
        let n = self.ops;
        self.ops += 1;
        let in_state = from >= STATE_AT;
        let nth_state = self.state_erases;
        if in_state {
            self.state_erases += 1;
        }
        match self.cut {
            Some(Cut::BeforeOp(k)) if k == n => return Err(self.die()),
            Some(Cut::StateErase { nth, tear }) if in_state && nth == nth_state => {
                self.tear_erase(from, to, tear);
                return Err(self.die());
            }
            _ => {}
        }
        self.mem[from..to].fill(0xFF);
        self.faulty[from / WRITE..to / WRITE].fill(false);
        Ok(())
    }

    fn tear_erase(&mut self, from: usize, to: usize, tear: Tear) {
        let words = (to - from) / WRITE;
        let mut rng = match tear {
            Tear::Bits(seed) => seed | 1,
            _ => 1,
        };
        let mut next = move || {
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            rng
        };
        for i in 0..words {
            let a = from + i * WRITE;
            match tear {
                Tear::Words(mask) => {
                    if mask & (1 << i) != 0 {
                        self.mem[a..a + WRITE].fill(0xFF);
                    }
                }
                Tear::Bits(_) => match next() % 4 {
                    0 => self.mem[a..a + WRITE].fill(0xFF),
                    1 => {
                        for j in 0..WRITE {
                            self.mem[a + j] |= next() as u8;
                        }
                    }
                    _ => {}
                },
                Tear::Ecc { erased } => {
                    if i < erased {
                        self.mem[a..a + WRITE].fill(0xFF);
                    } else if i == erased {
                        self.faulty[a / WRITE] = true;
                    }
                }
            }
        }
    }
}

/// A window of the flash as a `NorFlash`.
#[derive(Clone)]
pub struct Part {
    flash: Shared,
    at: usize,
    len: usize,
}

impl Part {
    /// ACTIVE.
    pub fn active(flash: &Shared) -> Self {
        Part {
            flash: flash.clone(),
            at: ACTIVE_AT,
            len: N * ERASE,
        }
    }
    /// DFU.
    pub fn dfu(flash: &Shared) -> Self {
        Part {
            flash: flash.clone(),
            at: DFU_AT,
            len: (N + 1) * ERASE,
        }
    }
    /// STATE.
    pub fn state(flash: &Shared) -> Self {
        Part {
            flash: flash.clone(),
            at: STATE_AT,
            len: ERASE,
        }
    }
}

impl ErrorType for Part {
    type Error = FlashError;
}

impl ReadNorFlash for Part {
    const READ_SIZE: usize = 1;
    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), FlashError> {
        if offset as usize + bytes.len() > self.len {
            return Err(FlashError::Bounds);
        }
        self.flash.borrow().read(self.at + offset as usize, bytes)
    }
    fn capacity(&self) -> usize {
        self.len
    }
}

impl NorFlash for Part {
    const WRITE_SIZE: usize = WRITE;
    const ERASE_SIZE: usize = ERASE;
    fn erase(&mut self, from: u32, to: u32) -> Result<(), FlashError> {
        if to as usize > self.len || from > to {
            return Err(FlashError::Bounds);
        }
        self.flash
            .borrow_mut()
            .erase(self.at + from as usize, self.at + to as usize)
    }
    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), FlashError> {
        if offset as usize + bytes.len() > self.len {
            return Err(FlashError::Bounds);
        }
        self.flash.borrow_mut().program(self.at + offset as usize, bytes)
    }
}

/// The suggested fix as a wrapper: zero the magic word before any erase of the state partition, so a torn erase
/// can never leave "SWAP" next to half-erased progress. With `mask_ecc`, a word that faults on read reads as
/// zeros (what chunky's `Masked` does for ECC flash).
#[derive(Clone)]
pub struct ZeroMagicFirst {
    /// The state partition.
    pub inner: Part,
    /// Reads of faulty words return zeros instead of an error.
    pub mask_ecc: bool,
}

impl ErrorType for ZeroMagicFirst {
    type Error = FlashError;
}

impl ReadNorFlash for ZeroMagicFirst {
    const READ_SIZE: usize = 1;
    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), FlashError> {
        match self.inner.read(offset, bytes) {
            Err(FlashError::Ecc) if self.mask_ecc => {
                // Word by word, so the readable neighbours keep their value.
                for (i, b) in bytes.iter_mut().enumerate() {
                    let mut one = [0u8];
                    *b = match self.inner.read(offset + i as u32, &mut one) {
                        Ok(()) => one[0],
                        Err(_) => 0,
                    };
                }
                Ok(())
            }
            r => r,
        }
    }
    fn capacity(&self) -> usize {
        self.inner.capacity()
    }
}

impl NorFlash for ZeroMagicFirst {
    const WRITE_SIZE: usize = WRITE;
    const ERASE_SIZE: usize = ERASE;
    fn erase(&mut self, from: u32, to: u32) -> Result<(), FlashError> {
        if from == 0 && to > 0 {
            self.inner.write(0, &[0; WRITE])?;
        }
        self.inner.erase(from, to)
    }
    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), FlashError> {
        self.inner.write(offset, bytes)
    }
}

/// Fill byte of page `p` of the old image.
pub fn old_page(p: usize) -> u8 {
    0xA0 + p as u8
}

/// Fill byte of page `p` of the new image.
pub fn new_page(p: usize) -> u8 {
    0xB0 + p as u8
}

/// Writes the old image into ACTIVE and the new one into the first N pages of DFU.
pub fn provision(flash: &Shared) {
    let mut f = flash.borrow_mut();
    for p in 0..N {
        f.mem[ACTIVE_AT + p * ERASE..][..ERASE].fill(old_page(p));
        f.mem[DFU_AT + p * ERASE..][..ERASE].fill(new_page(p));
    }
}

/// The pages of the image whose page `p` is filled with `page(p)`.
pub fn image_pages(page: fn(usize) -> u8) -> Vec<Vec<u8>> {
    (0..N).map(|p| vec![page(p); ERASE]).collect()
}

fn pages(f: &Flash, at: usize, n: usize) -> Vec<&[u8]> {
    (0..n).map(|p| &f.mem[at + p * ERASE..][..ERASE]).collect()
}

/// Classifies ACTIVE ([`classify::active`] on the pages).
pub fn active(flash: &Shared) -> Active {
    let f = flash.borrow();
    classify::active(&pages(&f, ACTIVE_AT, N), &image_pages(old_page), &image_pages(new_page))
}

/// Whether every page of the image (given by its fill function) still exists somewhere in ACTIVE or DFU.
pub fn image_survives(flash: &Shared, page: fn(usize) -> u8) -> bool {
    let f = flash.borrow();
    classify::survives(&image_pages(page), &pages(&f, ACTIVE_AT, N), &pages(&f, DFU_AT, N + 1))
}
