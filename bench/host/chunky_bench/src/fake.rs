//! A bench without hardware: [`FakePower`] and [`FakeDut`] share one simulated B129A. Its "firmware" is the real
//! embassy-boot 0.7.0 (`prepare_boot`, `mark_booted`) on a flash model with the G0's geometry, with the stock
//! STATE partition or a `Guarded`-like wrapper (zero the magic before the erase, ECC faults read as zeros), so
//! the run loop, the sweep and the CSV are tested end to end.
//!
//! Timing of a cut, as on the bench: PA6 rises `pre_trigger_us` before the STATE erase, the glitcher cuts `delay`
//! after that, the DUT goes dark `t_dead` later. Dark before the erase: nothing happened. Dark after it: the boot
//! completes, power still drops. In between: an ECC tear (embassy_torn's `Tear::Ecc`): the first words of the page
//! erased in proportion to the time, the next one faults on every read, the rest untouched. Whether the real G0
//! erases like that is what the bench is for; the model is only there to exercise the harness.

use crate::dut::{Dut, flash_regs};
use crate::fw::{Firmware, pages};
use crate::power::Power;
use anyhow::{Result, anyhow, bail};
use bench_layout::{
    ACTIVE, AppStage, BOOTLOADER, BootState, CONFIG, Config, DFU, FLASH_BASE, IMAGE_A, IMAGE_B, Mode, PAGE, REPORT,
    Region, Report, STATE, WRITE,
};
use embassy_boot::{BlockingFirmwareState, BootLoader, BootLoaderConfig, State};
use embassy_torn::classify::{Active, active};
use embedded_storage::nor_flash::{ErrorType, NorFlash, NorFlashError, NorFlashErrorKind, ReadNorFlash};
use std::cell::RefCell;
use std::rc::Rc;
use std::time::Duration;

/// The model's timing.
#[derive(Clone, Copy, Debug)]
pub struct SimParams {
    /// One page erase.
    pub t_erase: Duration,
    /// From the cut until the MCU stops.
    pub t_dead: Duration,
    /// `t_dead` varies by up to this much either way from run to run (deterministic, seeded).
    pub jitter: Duration,
    pub seed: u64,
}

impl Default for SimParams {
    fn default() -> Self {
        SimParams {
            t_erase: Duration::from_millis(22),
            t_dead: Duration::from_millis(8),
            jitter: Duration::ZERO,
            seed: 1,
        }
    }
}

const W: usize = WRITE as usize;
const TOTAL: usize = (DFU.at + DFU.len - FLASH_BASE) as usize;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SimError {
    PowerLoss,
    Ecc,
    Bounds,
}

impl NorFlashError for SimError {
    fn kind(&self) -> NorFlashErrorKind {
        match self {
            SimError::Bounds => NorFlashErrorKind::OutOfBounds,
            _ => NorFlashErrorKind::Other,
        }
    }
}

struct Cut {
    delay: Duration,
}

/// Flash and the power state of the model.
struct Cell {
    mem: Vec<u8>,
    faulty: Vec<bool>,
    eccr: u32,
    sr: u32,
    params: SimParams,
    rng: u64,
    cut: Option<Cut>,
    fired: bool,
    dead: bool,
    /// The cut came after the erase: the boot goes on, then power drops.
    dying: bool,
    /// ECC double errors the firmware's own reads hit this boot (stock: the core takes an NMI).
    ecc_hits: u32,
    /// ECC faults the guarded reader masked this boot, and the last address.
    masked: u32,
    masked_at: u32,
    state_erases: u32,
}

type Shared = Rc<RefCell<Cell>>;

impl Cell {
    fn range(&self, addr: u32, len: usize) -> Result<usize, SimError> {
        let off = addr.checked_sub(FLASH_BASE).ok_or(SimError::Bounds)? as usize;
        if off + len > TOTAL {
            return Err(SimError::Bounds);
        }
        Ok(off)
    }

    fn first_fault(&self, off: usize, len: usize) -> Option<usize> {
        (off / W..(off + len).div_ceil(W)).find(|&w| self.faulty[w])
    }

    fn note_ecc(&mut self, word: usize) {
        self.eccr = flash_regs::ECCD | (word as u32 & 0x3FFF);
    }

    fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<(), SimError> {
        if self.dead {
            return Err(SimError::PowerLoss);
        }
        let off = self.range(addr, buf.len())?;
        if let Some(w) = self.first_fault(off, buf.len()) {
            self.note_ecc(w);
            return Err(SimError::Ecc);
        }
        buf.copy_from_slice(&self.mem[off..off + buf.len()]);
        Ok(())
    }

    fn program(&mut self, addr: u32, data: &[u8]) -> Result<(), SimError> {
        if self.dead {
            return Err(SimError::PowerLoss);
        }
        let off = self.range(addr, data.len())?;
        if !off.is_multiple_of(W) || !data.len().is_multiple_of(W) {
            return Err(SimError::Bounds);
        }
        for (i, chunk) in data.chunks(W).enumerate() {
            let at = off + i * W;
            let used = self.mem[at..at + W].iter().any(|&b| b != 0xFF) || self.faulty[at / W];
            if used && chunk.iter().any(|&b| b != 0) {
                // G0: programming a double word that is not erased is refused, unless it is all zeros.
                self.sr |= flash_regs::PROGERR;
                continue;
            }
            for (j, &b) in chunk.iter().enumerate() {
                self.mem[at + j] &= b;
            }
            if used {
                self.faulty[at / W] = false;
            }
        }
        Ok(())
    }

    fn erase_page(&mut self, off: usize) {
        self.mem[off..off + PAGE as usize].fill(0xFF);
        self.faulty[off / W..(off + PAGE as usize) / W].fill(false);
    }

    fn next_jitter(&mut self) -> i128 {
        let j = self.params.jitter.as_nanos() as i128;
        if j == 0 {
            return 0;
        }
        self.rng ^= self.rng << 13;
        self.rng ^= self.rng >> 7;
        self.rng ^= self.rng << 17;
        (self.rng % (2 * j as u64 + 1)) as i128 - j
    }

    /// An erase of `[from, to)`; `state` marks the STATE partition, where PA6 rises first.
    fn erase(&mut self, from: u32, to: u32, state: bool, pre_trigger: Duration) -> Result<(), SimError> {
        if self.dead {
            return Err(SimError::PowerLoss);
        }
        let off = self.range(from, (to - from) as usize)?;
        if !off.is_multiple_of(PAGE as usize) || !(to - from).is_multiple_of(PAGE) {
            return Err(SimError::Bounds);
        }
        if state {
            self.state_erases += 1;
        }
        if let Some(cut) = self.cut.take_if(|_| state) {
            self.fired = true;
            let die_at = cut.delay.as_nanos() as i128 + self.params.t_dead.as_nanos() as i128 + self.next_jitter();
            let start = pre_trigger.as_nanos() as i128;
            let len = self.params.t_erase.as_nanos() as i128;
            if die_at <= start {
                self.dead = true;
                return Err(SimError::PowerLoss);
            }
            if die_at < start + len {
                // One page (STATE is one page): words erased in proportion, the next one torn.
                let words = PAGE as usize / W;
                let k = ((die_at - start) * words as i128 / len) as usize;
                self.mem[off..off + k * W].fill(0xFF);
                self.faulty[off / W..off / W + k].fill(false);
                if k < words {
                    self.faulty[off / W + k] = true;
                }
                self.dead = true;
                return Err(SimError::PowerLoss);
            }
            self.dying = true;
        }
        for p in (off..off + (to - from) as usize).step_by(PAGE as usize) {
            self.erase_page(p);
        }
        Ok(())
    }
}

/// A partition of the model as embassy-boot sees it.
struct Part {
    cell: Shared,
    region: Region,
    state: bool,
    pre_trigger: Duration,
}

impl ErrorType for Part {
    type Error = SimError;
}

impl ReadNorFlash for Part {
    const READ_SIZE: usize = 1;
    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), SimError> {
        if offset as usize + bytes.len() > self.region.len as usize {
            return Err(SimError::Bounds);
        }
        let mut c = self.cell.borrow_mut();
        let r = c.read(self.region.at + offset, bytes);
        if r == Err(SimError::Ecc) {
            c.ecc_hits += 1;
        }
        r
    }
    fn capacity(&self) -> usize {
        self.region.len as usize
    }
}

impl NorFlash for Part {
    const WRITE_SIZE: usize = W;
    const ERASE_SIZE: usize = PAGE as usize;
    fn erase(&mut self, from: u32, to: u32) -> Result<(), SimError> {
        if to > self.region.len || from > to {
            return Err(SimError::Bounds);
        }
        let at = self.region.at;
        self.cell
            .borrow_mut()
            .erase(at + from, at + to, self.state, self.pre_trigger)
    }
    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), SimError> {
        if offset as usize + bytes.len() > self.region.len as usize {
            return Err(SimError::Bounds);
        }
        self.cell.borrow_mut().program(self.region.at + offset, bytes)
    }
}

/// The guarded STATE partition: magic zeroed before an erase, ECC faults read as zeros.
struct Guarded(Part);

impl ErrorType for Guarded {
    type Error = SimError;
}

impl ReadNorFlash for Guarded {
    const READ_SIZE: usize = 1;
    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), SimError> {
        if offset as usize + bytes.len() > self.0.region.len as usize {
            return Err(SimError::Bounds);
        }
        let mut c = self.0.cell.borrow_mut();
        let at = self.0.region.at + offset;
        match c.read(at, bytes) {
            Err(SimError::Ecc) => {
                for (i, b) in bytes.iter_mut().enumerate() {
                    let mut one = [0u8];
                    *b = match c.read(at + i as u32, &mut one) {
                        Ok(()) => one[0],
                        Err(SimError::Ecc) => {
                            c.masked += 1;
                            c.masked_at = at + i as u32;
                            0
                        }
                        Err(e) => return Err(e),
                    };
                }
                Ok(())
            }
            r => r,
        }
    }
    fn capacity(&self) -> usize {
        self.0.capacity()
    }
}

impl NorFlash for Guarded {
    const WRITE_SIZE: usize = W;
    const ERASE_SIZE: usize = PAGE as usize;
    fn erase(&mut self, from: u32, to: u32) -> Result<(), SimError> {
        if from == 0 && to > 0 {
            self.0.write(0, &[0; W])?;
        }
        self.0.erase(from, to)
    }
    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), SimError> {
        self.0.write(offset, bytes)
    }
}

fn prepare<S: NorFlash>(active: Part, dfu: Part, state: S) -> Result<State, ()> {
    let mut buf = [0u8; 256];
    BootLoader::new(BootLoaderConfig { active, dfu, state })
        .prepare_boot(&mut buf)
        .map_err(|_| ())
}

/// How one boot ended.
enum Step {
    /// Power went away (the cut).
    Dead,
    /// The application reset itself.
    Reset,
    /// Running (or hanging) until something from outside happens.
    Settled,
}

enum Fault {
    Dead,
    Nmi,
    Error,
}

/// The simulated DUT.
pub struct Sim {
    cell: Shared,
    ram: Vec<u8>,
    xpsr: u32,
    pending: bool,
    image_a: Vec<Vec<u8>>,
    image_b: Vec<Vec<u8>>,
    /// Boots run so far (for tests).
    pub boots: usize,
}

impl Sim {
    fn part(&self, region: Region, state: bool, pre_trigger: Duration) -> Part {
        Part {
            cell: self.cell.clone(),
            region,
            state,
            pre_trigger,
        }
    }

    fn lose_power(&mut self) {
        self.ram.fill(0);
        let mut c = self.cell.borrow_mut();
        c.dead = false;
        c.dying = false;
    }

    /// Runs pending boots until the device settles (power cuts in between come back after their `off`).
    fn settle(&mut self) {
        if !self.pending {
            return;
        }
        self.pending = false;
        for _ in 0..16 {
            self.boots += 1;
            match self.boot_once() {
                Step::Dead => self.lose_power(),
                Step::Reset => {}
                Step::Settled if self.cell.borrow().dying => self.lose_power(),
                Step::Settled => return,
            }
        }
    }

    fn write_report(&mut self, r: &Report) {
        self.ram[..Report::BYTES].copy_from_slice(&r.encode());
    }

    fn boot_once(&mut self) -> Step {
        self.xpsr = 0x0100_0000;
        {
            let mut c = self.cell.borrow_mut();
            c.ecc_hits = 0;
            c.masked = 0;
            c.state_erases = 0;
        }
        let (variant, config) = {
            let c = self.cell.borrow();
            let cfg_off = (CONFIG.at - FLASH_BASE) as usize;
            (
                c.mem[(BOOTLOADER.at - FLASH_BASE) as usize],
                Config::read_or_default(&c.mem[cfg_off..cfg_off + Config::BYTES]),
            )
        };
        let guarded = match variant {
            v if v == bench_layout::Variant::Stock as u8 => false,
            v if v == bench_layout::Variant::Guarded as u8 => true,
            _ => {
                // No bootloader we know: it faults before writing a report.
                self.xpsr = 3;
                return Step::Settled;
            }
        };
        let mut report = Report {
            variant,
            ..Report::EMPTY
        };
        self.write_report(&report);
        if config.mode == Mode::Measure {
            return Step::Settled;
        }
        let pre = Duration::from_micros(u64::from(config.pre_trigger_us));
        let state = self.part(STATE, true, pre);
        let (a, d) = (self.part(ACTIVE, false, pre), self.part(DFU, false, pre));
        let booted = if guarded {
            prepare(a, d, Guarded(state))
        } else {
            prepare(a, d, state)
        };
        match self.fault_after(booted.is_err(), guarded) {
            Some(Fault::Dead) => return Step::Dead,
            Some(Fault::Nmi) => return Step::Settled,
            Some(Fault::Error) => {
                report.boot_state = BootState::Error as u8;
                self.finish(&mut report);
                return Step::Settled;
            }
            None => {}
        }
        report.boot_state = match booted {
            Ok(State::Swap) => BootState::Swap,
            Ok(State::Revert) => BootState::Revert,
            Ok(State::DfuDetach) => BootState::DfuDetach,
            _ => BootState::Boot,
        } as u8;
        self.finish(&mut report);

        // The application: whichever image ACTIVE holds.
        let now: Vec<Vec<u8>> = {
            let c = self.cell.borrow();
            let at = (ACTIVE.at - FLASH_BASE) as usize;
            pages(&c.mem[at..at + ACTIVE.len as usize])
                .into_iter()
                .map(<[u8]>::to_vec)
                .collect()
        };
        let image = match active(&now, &self.image_a, &self.image_b) {
            Active::Old => IMAGE_A,
            Active::New => IMAGE_B,
            Active::Mixed(_) => {
                self.xpsr = 3;
                return Step::Settled;
            }
        };
        report.app_image = image;
        report.app_stage = AppStage::Running as u8;
        self.finish(&mut report);
        if config.mode == Mode::Provision && image == IMAGE_A {
            // A stages B with the official API, then stops (a breakpoint).
            let mut al = [0u8; W];
            let state = self.part(STATE, true, pre);
            let r = if guarded {
                BlockingFirmwareState::new(Guarded(state), &mut al)
                    .mark_updated()
                    .map_err(|_| ())
            } else {
                BlockingFirmwareState::new(state, &mut al)
                    .mark_updated()
                    .map_err(|_| ())
            };
            report.app_stage = if r.is_ok() {
                AppStage::Confirmed
            } else {
                AppStage::Error
            } as u8;
            self.finish(&mut report);
            return Step::Settled;
        }
        if image == IMAGE_A || config.mode != Mode::Run {
            return Step::Settled;
        }
        if !config.confirm {
            report.app_stage = AppStage::Resetting as u8;
            self.finish(&mut report);
            return Step::Reset;
        }
        let mut al = [0u8; W];
        let state = self.part(STATE, true, pre);
        let r = if guarded {
            BlockingFirmwareState::new(Guarded(state), &mut al)
                .mark_booted()
                .map_err(|_| ())
        } else {
            BlockingFirmwareState::new(state, &mut al).mark_booted().map_err(|_| ())
        };
        match self.fault_after(r.is_err(), guarded) {
            Some(Fault::Dead) => return Step::Dead,
            Some(Fault::Nmi) => return Step::Settled,
            Some(Fault::Error) => {
                report.app_stage = AppStage::Error as u8;
                self.finish(&mut report);
                return Step::Settled;
            }
            None => {}
        }
        report.app_stage = AppStage::Confirmed as u8;
        self.finish(&mut report);
        Step::Settled
    }

    /// After a firmware step: power gone, an NMI (stock reader hit an ECC double error: the default handler
    /// hangs, the report stays as it was), or an error the firmware reports.
    fn fault_after(&mut self, failed: bool, guarded: bool) -> Option<Fault> {
        let (dead, hits) = {
            let c = self.cell.borrow();
            (c.dead, c.ecc_hits)
        };
        if dead {
            Some(Fault::Dead)
        } else if hits > 0 && !guarded {
            self.xpsr = 2;
            Some(Fault::Nmi)
        } else {
            failed.then_some(Fault::Error)
        }
    }

    fn finish(&mut self, report: &mut Report) {
        {
            let c = self.cell.borrow();
            report.ecc_faults = c.masked;
            report.ecc_last_addr = if c.masked > 0 { c.masked_at } else { 0 };
            report.state_erases = c.state_erases;
            if c.state_erases > 0 {
                report.erase_us = c.params.t_erase.as_micros() as u32;
            }
        }
        let r = *report;
        self.write_report(&r);
    }
}

/// A simulated bench: one DUT, its power switch and its debug probe.
#[derive(Clone)]
pub struct FakeBench {
    pub sim: Rc<RefCell<Sim>>,
}

impl FakeBench {
    /// Blank flash; images A and B as the firmware knows them (to tell what ACTIVE runs).
    pub fn new(fw: &Firmware, params: SimParams) -> FakeBench {
        let to_pages = |img: &[u8]| pages(img).into_iter().map(<[u8]>::to_vec).collect();
        let cell = Cell {
            mem: vec![0xFF; TOTAL],
            faulty: vec![false; TOTAL / W],
            eccr: 0,
            sr: 0,
            rng: params.seed | 1,
            params,
            cut: None,
            fired: false,
            dead: false,
            dying: false,
            ecc_hits: 0,
            masked: 0,
            masked_at: 0,
            state_erases: 0,
        };
        FakeBench {
            sim: Rc::new(RefCell::new(Sim {
                cell: Rc::new(RefCell::new(cell)),
                ram: vec![0; REPORT.len as usize],
                xpsr: 0,
                pending: false,
                image_a: to_pages(&fw.app_a),
                image_b: to_pages(&fw.app_b),
                boots: 0,
            })),
        }
    }

    pub fn power(&self) -> FakePower {
        FakePower { sim: self.sim.clone() }
    }

    pub fn dut(&self) -> FakeDut {
        FakeDut {
            sim: self.sim.clone(),
            attached: false,
        }
    }
}

pub struct FakePower {
    sim: Rc<RefCell<Sim>>,
}

impl Power for FakePower {
    fn power_cycle(&mut self, _off: Duration) -> Result<()> {
        let mut s = self.sim.borrow_mut();
        s.settle();
        s.lose_power();
        let mut c = s.cell.borrow_mut();
        c.cut = None;
        c.fired = true;
        drop(c);
        s.pending = true;
        Ok(())
    }

    fn arm_cut(&mut self, delay: Duration, _off: Duration) -> Result<()> {
        let s = self.sim.borrow();
        let mut c = s.cell.borrow_mut();
        c.cut = Some(Cut { delay });
        c.fired = false;
        Ok(())
    }

    fn fired(&mut self) -> Result<bool> {
        Ok(self.sim.borrow().cell.borrow().fired)
    }

    fn disarm(&mut self) -> Result<()> {
        self.sim.borrow().cell.borrow_mut().cut = None;
        Ok(())
    }

    fn sleep(&mut self, _d: Duration) {
        self.sim.borrow_mut().settle();
    }
}

pub struct FakeDut {
    sim: Rc<RefCell<Sim>>,
    attached: bool,
}

impl FakeDut {
    fn check(&self) -> Result<()> {
        if !self.attached {
            bail!("not attached");
        }
        Ok(())
    }
}

impl Dut for FakeDut {
    fn attach(&mut self) -> Result<()> {
        self.sim.borrow_mut().settle();
        self.attached = true;
        Ok(())
    }

    fn detach(&mut self) {
        self.attached = false;
    }

    fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<()> {
        self.check()?;
        let s = self.sim.borrow();
        if addr >= REPORT.at && addr as u64 + buf.len() as u64 <= REPORT.end() as u64 {
            let off = (addr - REPORT.at) as usize;
            buf.copy_from_slice(&s.ram[off..off + buf.len()]);
            return Ok(());
        }
        s.cell
            .borrow_mut()
            .read(addr, buf)
            .map_err(|e| anyhow!("read {addr:#010x}+{}: {e:?}", buf.len()))
    }

    fn read_u32(&mut self, addr: u32) -> Result<u32> {
        self.check()?;
        let s = self.sim.borrow();
        let c = s.cell.borrow();
        match addr {
            flash_regs::ECCR => Ok(c.eccr),
            flash_regs::SR => Ok(c.sr),
            _ => {
                drop(c);
                drop(s);
                let mut b = [0u8; 4];
                self.read(addr, &mut b)?;
                Ok(u32::from_le_bytes(b))
            }
        }
    }

    fn write_u32(&mut self, addr: u32, value: u32) -> Result<()> {
        self.check()?;
        let s = self.sim.borrow();
        let mut c = s.cell.borrow_mut();
        match addr {
            flash_regs::ECCR => {
                let clear = value & (flash_regs::ECCC | flash_regs::ECCD);
                c.eccr &= !clear;
                if c.eccr & (flash_regs::ECCC | flash_regs::ECCD) == 0 {
                    c.eccr = 0;
                }
                Ok(())
            }
            flash_regs::SR => {
                c.sr &= !value;
                Ok(())
            }
            // Vector catch: the model's core always runs after a reset.
            crate::run::DEMCR => Ok(()),
            _ => bail!("fake DUT: no register at {addr:#010x}"),
        }
    }

    fn program_pages(&mut self, list: &[(u32, Vec<u8>)]) -> Result<()> {
        self.check()?;
        let s = self.sim.borrow();
        let mut c = s.cell.borrow_mut();
        for (at, data) in list {
            if !at.is_multiple_of(PAGE) || data.len() != PAGE as usize {
                bail!("not a page: {at:#010x}+{}", data.len());
            }
            let off = c.range(*at, data.len()).map_err(|e| anyhow!("{e:?}"))?;
            c.erase_page(off);
            c.mem[off..off + data.len()].copy_from_slice(data);
        }
        Ok(())
    }

    fn reset_run(&mut self) -> Result<()> {
        Ok(())
    }

    fn halt(&mut self) -> Result<u32> {
        self.check()?;
        Ok(self.sim.borrow().xpsr)
    }
}
