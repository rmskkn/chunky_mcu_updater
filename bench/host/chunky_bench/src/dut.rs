//! The device under test over SWD: [`Dut`] is the few raw operations the bench needs, [`ProbeDut`] does them
//! with probe-rs (Donguru's CMSIS-DAP v2), [`crate::fake`] on a model. Restoring pages and reading a run back
//! are generic on top: [`restore`], [`read_back`].

use crate::fw::Target;
use anyhow::{Context, Result};
use bench_layout::{ACTIVE, DFU, PAGE, REPORT, Report, STATE, WRITE};

/// STM32G0 FLASH registers (RM0444).
pub mod flash_regs {
    pub const SR: u32 = 0x4002_2010;
    pub const ECCR: u32 = 0x4002_2018;
    /// ECC correction (single error, corrected) flag; write 1 to clear.
    pub const ECCC: u32 = 1 << 30;
    /// ECC detection (double error) flag; write 1 to clear.
    pub const ECCD: u32 = 1 << 31;
    /// ECC correction interrupt enable: kept when clearing.
    pub const ECCCIE: u32 = 1 << 24;
    /// FLASH_SR PROGERR: a program over a word that was not erased (other than zeros).
    pub const PROGERR: u32 = 1 << 3;
}

/// Raw access to the DUT. Addresses are absolute.
pub trait Dut {
    /// Connects over SWD without resetting the core.
    fn attach(&mut self) -> Result<()>;
    /// Lets go of SWD (Donguru floats SWD and CC); the core keeps whatever state it is in.
    fn detach(&mut self);
    /// Reads memory; an error for a range the bus refuses (flash with an ECC double error).
    fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<()>;
    fn read_u32(&mut self, addr: u32) -> Result<u32>;
    fn write_u32(&mut self, addr: u32, value: u32) -> Result<()>;
    /// Erases and programs whole flash pages: `(address, PAGE bytes)`, page-aligned.
    fn program_pages(&mut self, pages: &[(u32, Vec<u8>)]) -> Result<()>;
    /// Halts the core and returns its xPSR.
    fn halt(&mut self) -> Result<u32>;
}

/// A region read back, with the double words the bus refused (filled with [`UNREADABLE`]).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Dump {
    pub data: Vec<u8>,
    /// Offsets (in the region) of unreadable write units.
    pub unreadable: Vec<u32>,
}

/// What an unreadable byte reads as in a dump.
pub const UNREADABLE: u8 = 0xEE;

/// Reads `len` bytes at `addr`, falling back to pages and then write units where the bus refuses a read.
pub fn read_tolerant(dut: &mut dyn Dut, addr: u32, len: u32) -> Result<Dump> {
    let mut d = Dump {
        data: vec![0; len as usize],
        unreadable: vec![],
    };
    if dut.read(addr, &mut d.data).is_ok() {
        return Ok(d);
    }
    for page in (0..len).step_by(PAGE as usize) {
        let n = PAGE.min(len - page) as usize;
        let at = page as usize;
        if dut.read(addr + page, &mut d.data[at..at + n]).is_ok() {
            continue;
        }
        for w in (0..n as u32).step_by(WRITE as usize) {
            let wa = at + w as usize;
            let wn = (WRITE as usize).min(n - w as usize);
            if dut.read(addr + page + w, &mut d.data[wa..wa + wn]).is_err() {
                d.data[wa..wa + wn].fill(UNREADABLE);
                d.unreadable.push(page + w);
            }
        }
    }
    Ok(d)
}

/// Brings flash to `targets`, writing only the pages that differ; returns how many it wrote. Then reads them
/// back and fails if one still differs.
pub fn restore(dut: &mut dyn Dut, targets: &[Target]) -> Result<usize> {
    let mut todo = vec![];
    for t in targets {
        let now = read_tolerant(dut, t.region.at, t.region.len)?;
        for (i, (want, have)) in t
            .data
            .chunks(PAGE as usize)
            .zip(now.data.chunks(PAGE as usize))
            .enumerate()
        {
            let at = t.region.at + i as u32 * PAGE;
            let torn = now.unreadable.iter().any(|&u| u / PAGE == i as u32);
            if want != have || torn {
                todo.push((at, want.to_vec()));
            }
        }
    }
    if todo.is_empty() {
        return Ok(0);
    }
    dut.program_pages(&todo).context("programming flash")?;
    for (at, want) in &todo {
        let mut have = vec![0; want.len()];
        dut.read(*at, &mut have)
            .with_context(|| format!("verifying page {at:#010x}"))?;
        if &have != want {
            anyhow::bail!("page {at:#010x} does not hold what was programmed");
        }
    }
    clear_ecc(dut)?;
    Ok(todo.len())
}

/// Clears ECCD and ECCC in FLASH_ECCR (keeping the interrupt enable); returns the value before.
pub fn clear_ecc(dut: &mut dyn Dut) -> Result<u32> {
    use flash_regs::*;
    let v = dut.read_u32(ECCR)?;
    if v & (ECCC | ECCD) != 0 {
        dut.write_u32(ECCR, v & (ECCC | ECCD | ECCCIE))?;
    }
    Ok(v)
}

/// Everything read after one boot.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Readback {
    /// xPSR of the halted core (IPSR = the low 6 bits).
    pub xpsr: u32,
    /// The REPORT region as read (raw).
    pub report_raw: Vec<u8>,
    /// FLASH_ECCR as found, before the harness read flash (what the firmware ran into).
    pub eccr: u32,
    /// FLASH_ECCR after the harness read STATE, ACTIVE and DFU (what the debugger ran into), before clearing.
    pub eccr_read: u32,
    pub sr: u32,
    pub state: Dump,
    pub active: Dump,
    pub dfu: Dump,
}

impl Readback {
    pub fn ipsr(&self) -> u32 {
        self.xpsr & 0x3F
    }
    pub fn report(&self) -> Option<Report> {
        Report::decode(&self.report_raw)
    }
}

/// Halts the core and reads the report, IPSR, FLASH_ECCR/SR, STATE, ACTIVE and DFU; clears ECCR at the end.
/// ECCR is read before any flash so it shows what the firmware hit, not what the debugger hit.
pub fn read_back(dut: &mut dyn Dut) -> Result<Readback> {
    let xpsr = dut.halt().context("halting the core")?;
    let mut report_raw = vec![0; REPORT.len as usize];
    dut.read(REPORT.at, &mut report_raw).context("reading the report")?;
    let eccr = dut.read_u32(flash_regs::ECCR)?;
    let sr = dut.read_u32(flash_regs::SR)?;
    let state = read_tolerant(dut, STATE.at, STATE.len)?;
    let active = read_tolerant(dut, ACTIVE.at, ACTIVE.len)?;
    let dfu = read_tolerant(dut, DFU.at, DFU.len)?;
    let eccr_read = clear_ecc(dut)?;
    Ok(Readback {
        xpsr,
        report_raw,
        eccr,
        eccr_read,
        sr,
        state,
        active,
        dfu,
    })
}
