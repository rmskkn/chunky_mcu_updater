//! One run of the bench (bench/README.md "One run"): restore, detach, power-cycle and arm, let the cut land, read
//! back, extra plain power cycles, classify.

use crate::dut::{Dut, Readback, read_back, restore};
use crate::fw::{Firmware, pages};
use crate::power::Power;
use anyhow::{Context, Result};
use bench_layout::{Config, Mode};
use embassy_torn::classify::{Class, classify};
use std::fmt;
use std::str::FromStr;
use std::time::Duration;

/// Which bootloader build.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Variant {
    /// embassy-boot 0.7.0 as it comes.
    Stock,
    /// STATE behind `chunky_swap::Guarded`.
    Guarded,
}

impl Variant {
    pub const ALL: [Variant; 2] = [Variant::Stock, Variant::Guarded];
    pub fn layout(self) -> bench_layout::Variant {
        match self {
            Variant::Stock => bench_layout::Variant::Stock,
            Variant::Guarded => bench_layout::Variant::Guarded,
        }
    }
    pub fn name(self) -> &'static str {
        match self {
            Variant::Stock => "stock",
            Variant::Guarded => "guarded",
        }
    }
}

/// Which STATE erase is cut.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Path {
    /// B runs unconfirmed and resets; the bootloader's revert erases STATE.
    Revert,
    /// B calls `mark_booted`; its STATE erase.
    MarkBooted,
}

impl Path {
    pub const ALL: [Path; 2] = [Path::Revert, Path::MarkBooted];
    pub fn name(self) -> &'static str {
        match self {
            Path::Revert => "revert",
            Path::MarkBooted => "mark_booted",
        }
    }
}

macro_rules! named {
    ($t:ty) => {
        impl fmt::Display for $t {
            fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str(self.name())
            }
        }
        impl FromStr for $t {
            type Err = anyhow::Error;
            fn from_str(s: &str) -> Result<Self> {
                <$t>::ALL
                    .into_iter()
                    .find(|v| v.name() == s)
                    .ok_or_else(|| anyhow::anyhow!("'{s}' is none of {:?}", <$t>::ALL.map(|v| v.name())))
            }
        }
    };
}
named!(Variant);
named!(Path);

/// Everything one run takes.
#[derive(Clone, Debug, PartialEq)]
pub struct RunParams {
    pub variant: Variant,
    pub path: Path,
    /// From PA6 rising to the cut.
    pub delay: Duration,
    /// PA6 goes high this long before the erase (config record).
    pub pre_trigger_us: u32,
    /// Power off time of the cut and of every plain power cycle.
    pub off: Duration,
    /// Bootloader waits this long first: the window to arm the glitcher in.
    pub boot_delay_ms: u32,
    /// Unconfirmed B resets after this long.
    pub reset_after_ms: u32,
    /// From arming (or a plain power cycle) until the DUT has settled and is read back.
    pub settle: Duration,
    /// Plain power cycles after the cut, each read back.
    pub boots_after: u32,
}

impl RunParams {
    pub fn config(&self) -> Config {
        Config {
            mode: Mode::Run,
            confirm: self.path == Path::MarkBooted,
            pre_trigger_us: self.pre_trigger_us,
            boot_delay_ms: self.boot_delay_ms,
            reset_after_ms: self.reset_after_ms,
            period_ms: Config::DEFAULT.period_ms,
        }
    }
}

/// Why a boot counts as a brick.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Brick {
    /// Core stuck in the NMI handler (IPSR 2): an ECC double error on a flash read, stock fault handlers.
    Nmi,
    /// Core stuck in HardFault (IPSR 3).
    HardFault,
    /// No report: nothing ran far enough to write it.
    NoReport,
}

/// One boot, classified.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Outcome {
    Brick(Brick),
    Flash(Class),
}

impl Outcome {
    /// `old`, `new`, `mixed`, `lost` or `brick`.
    pub fn name(&self) -> &'static str {
        match self {
            Outcome::Brick(_) => "brick",
            Outcome::Flash(c) => c.name(),
        }
    }
    fn bad(&self) -> bool {
        matches!(self, Outcome::Brick(_) | Outcome::Flash(Class::Mixed(_)))
    }
}

impl fmt::Display for Outcome {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Outcome::Brick(Brick::Nmi) => f.write_str("brick:nmi"),
            Outcome::Brick(Brick::HardFault) => f.write_str("brick:hardfault"),
            Outcome::Brick(Brick::NoReport) => f.write_str("brick:noreport"),
            Outcome::Flash(c) => c.fmt(f),
        }
    }
}

/// A boot's read-back, judged: brick when IPSR says NMI or HardFault or there is no report, else what flash
/// holds (`flash` is the flash class either way).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Boot {
    pub readback: Readback,
    pub flash: Class,
    pub outcome: Outcome,
}

pub fn judge(fw: &Firmware, rb: Readback) -> Boot {
    let flash = classify(
        &pages(&rb.active.data),
        &pages(&rb.dfu.data),
        &pages(&fw.app_a),
        &pages(&fw.app_b),
    );
    let outcome = match rb.ipsr() {
        2 => Outcome::Brick(Brick::Nmi),
        3 => Outcome::Brick(Brick::HardFault),
        _ if rb.report().is_none() => Outcome::Brick(Brick::NoReport),
        _ => Outcome::Flash(flash.clone()),
    };
    Boot {
        readback: rb,
        flash,
        outcome,
    }
}

/// The run as a whole, as embassy_torn folds its boots: the first brick or mixture wins; else `lost` if any boot
/// saw an image lost; else what the last boot ran.
pub fn fold(boots: &[Boot]) -> Option<Outcome> {
    if let Some(b) = boots.iter().find(|b| b.outcome.bad()) {
        return Some(b.outcome.clone());
    }
    if boots.iter().any(|b| b.outcome == Outcome::Flash(Class::Lost)) {
        return Some(Outcome::Flash(Class::Lost));
    }
    boots.last().map(|b| b.outcome.clone())
}

/// What one run did.
#[derive(Clone, Debug)]
pub struct RunRecord {
    pub params: RunParams,
    /// Pages the restore had to write.
    pub restored: usize,
    /// The glitcher's armed cut fired.
    pub fired: bool,
    /// The boot after the cut, then one per extra power cycle.
    pub boots: Vec<Boot>,
    pub outcome: Outcome,
}

/// One run: steps 1-8 of bench/README.md.
pub fn run_once(power: &mut dyn Power, dut: &mut dyn Dut, fw: &Firmware, p: &RunParams) -> Result<RunRecord> {
    power.disarm()?;
    dut.attach().context("attaching for the restore")?;
    let restored = restore(dut, &fw.targets(p.variant, &p.config()));
    dut.detach();
    let restored = restored.context("restoring S0")?;

    power.power_cycle(p.off)?;
    power.arm_cut(p.delay, p.off)?;
    let mut boots = vec![];
    let mut fired = false;
    for n in 0..=p.boots_after {
        if n > 0 {
            power.power_cycle(p.off)?;
        }
        power.sleep(p.settle);
        if n == 0 {
            // Before a plain power cycle re-arms the glitcher and resets its status.
            fired = power.fired()?;
            power.disarm()?;
        }
        dut.attach().with_context(|| format!("attaching after boot {n}"))?;
        let rb = read_back(dut);
        dut.detach();
        boots.push(judge(fw, rb.with_context(|| format!("reading back boot {n}"))?));
    }
    let outcome = fold(&boots).expect("at least one boot");
    Ok(RunRecord {
        params: p.clone(),
        restored,
        fired,
        boots,
        outcome,
    })
}
