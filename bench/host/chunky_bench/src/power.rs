//! What stops the DUT: the Xyloni glitcher's `od_out` (GLT-8). As a power switch it is push-pull on Donguru's
//! Q101 gate: idle high = powered, low = cut. As a reset line it is open-drain on the DUT's NRST: low for a
//! short pulse, never driven high ([`Cut`]). [`Power`] is what the run loop needs; [`Glitcher`] does it on the bench, [`crate::fake`]
//! in the tests.

use anyhow::{Result, bail};
use glitch::{Action, Board, Drive, Dur, Edges, Level, Mark, Setup, Trigger};
use std::time::{Duration, Instant};

/// How the DUT is stopped in the middle of the erase.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum Cut {
    /// Power off for `off` (Q102 opens, 3V3 decays through the DUT).
    #[default]
    Power,
    /// NRST pulled low for `width` (open-drain); power stays on.
    Reset { width: Duration },
}

impl Cut {
    pub fn name(&self) -> &'static str {
        match self {
            Cut::Power => "power",
            Cut::Reset { .. } => "reset",
        }
    }
    /// The reset pulse width (zero for a power cut).
    pub fn width(&self) -> Duration {
        match self {
            Cut::Power => Duration::ZERO,
            Cut::Reset { width } => *width,
        }
    }
}

/// The power switch or reset line, and the bench's notion of time. With [`Cut::Reset`] wiring a "power cycle" is
/// one reset pulse and a "cut" is a reset pulse: `off` is then ignored, the DUT restarts at once.
pub trait Power {
    /// Cuts power now for `off` and returns once it is back on.
    fn power_cycle(&mut self, off: Duration) -> Result<()>;
    /// Arms one cut: `delay` after the next rising edge of the trigger (PA6), power off for `off`.
    fn arm_cut(&mut self, delay: Duration, off: Duration) -> Result<()>;
    /// Whether the armed cut fired.
    fn fired(&mut self) -> Result<bool>;
    /// Drops an armed cut that has not fired; power stays on.
    fn disarm(&mut self) -> Result<()>;
    /// Lets the DUT run for `d` (the fake runs its model instead of sleeping).
    fn sleep(&mut self, d: Duration) {
        std::thread::sleep(d);
    }
}

/// The glitcher on its UART link.
pub struct Glitcher {
    pub board: Board,
    pub cut: Cut,
}

fn setup(trigger: Trigger, delay: Duration) -> Setup {
    Setup {
        trigger,
        skip: 0,
        delay: Dur::Ns(delay.as_nanos() as f64),
        repeat: 1,
        interval: Dur::Cycles(1),
        endless: false,
        retrig: false,
        mark: Mark::Window,
    }
}

fn stuck_low(off: Duration) -> Action {
    Action::Stuck {
        width: Dur::Ns(off.as_nanos() as f64),
        level: Level::Low,
    }
}

impl Glitcher {
    /// Opens the glitcher (`port` or the first Xyloni found), sets the drive of `od_out` for `cut` (push-pull for
    /// the power switch: the DUT is powered from here on; open-drain for NRST: released) and disarms it.
    ///
    /// The bitstream comes up push-pull, idle high: on NRST wiring that drives the line high against the DUT's own
    /// reset driver until this runs, so set open-drain before moving the wire (bench/README.md).
    pub fn open(port: Option<&str>, cut: Cut) -> Result<Glitcher> {
        let mut board = Board::open(port)?;
        board.set_drive(match cut {
            Cut::Power => Drive::PushPull,
            Cut::Reset { .. } => Drive::OpenDrain,
        })?;
        board.disarm()?;
        Ok(Glitcher { board, cut })
    }

    /// How long `od_out` stays low: the power-off time, or the reset pulse.
    fn low(&self, off: Duration) -> Duration {
        match self.cut {
            Cut::Power => off,
            Cut::Reset { width } => width,
        }
    }

    /// The shortest delay the glitcher can do (3 clock cycles).
    pub fn min_delay(&self) -> Duration {
        Duration::from_nanos(3 * 1_000_000_000 / u64::from(self.board.clk_hz.max(1)))
    }
}

impl Power for Glitcher {
    fn power_cycle(&mut self, off: Duration) -> Result<()> {
        let min = self.min_delay();
        let off = self.low(off);
        self.board.arm(&setup(Trigger::Now, min), &stuck_low(off), false)?;
        let until = Instant::now() + off + Duration::from_secs(1);
        loop {
            let s = self.board.status()?;
            if s.fired && !s.active {
                return Ok(());
            }
            if Instant::now() > until {
                bail!(
                    "the glitcher did not finish the power cycle (state: {})",
                    s.state_name()
                );
            }
            std::thread::sleep(Duration::from_millis(2).min(off / 4 + Duration::from_micros(100)));
        }
    }

    fn arm_cut(&mut self, delay: Duration, off: Duration) -> Result<()> {
        // The glitcher cannot cut sooner than a few clock cycles after the edge.
        let delay = delay.max(self.min_delay());
        let off = self.low(off);
        self.board
            .arm(&setup(Trigger::External(Edges::Rise), delay), &stuck_low(off), false)?;
        Ok(())
    }

    fn fired(&mut self) -> Result<bool> {
        Ok(self.board.status()?.fired)
    }

    fn disarm(&mut self) -> Result<()> {
        self.board.disarm()
    }
}

/// Serial ports of Xyloni boards: (path, description), the glitcher's FPGA UART marked.
pub fn ports() -> Vec<(String, String)> {
    glitch::ports()
        .into_iter()
        .map(|p| {
            let what = if p.uart { "FPGA UART" } else { "other channel" };
            (p.path, format!("{}, {what}", p.product))
        })
        .collect()
}
