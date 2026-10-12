//! The DUT's power switch: the Xyloni glitcher's `od_out` (push-pull, GLT-8) on Donguru's Q101 gate. Idle high =
//! powered, low = cut. [`Power`] is what the run loop needs; [`Glitcher`] does it on the bench, [`crate::fake`]
//! in the tests.

use anyhow::{Result, bail};
use glitch::{Action, Board, Drive, Dur, Edges, Level, Mark, Setup, Trigger};
use std::time::{Duration, Instant};

/// The power switch, and the bench's notion of time.
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
    /// Opens the glitcher (`port` or the first Xyloni found), sets `od_out` push-pull and disarms it: the DUT is
    /// powered from here on.
    pub fn open(port: Option<&str>) -> Result<Glitcher> {
        let mut board = Board::open(port)?;
        board.set_drive(Drive::PushPull)?;
        board.disarm()?;
        Ok(Glitcher { board })
    }

    /// The shortest delay the glitcher can do (3 clock cycles).
    pub fn min_delay(&self) -> Duration {
        Duration::from_nanos(3 * 1_000_000_000 / u64::from(self.board.clk_hz.max(1)))
    }
}

impl Power for Glitcher {
    fn power_cycle(&mut self, off: Duration) -> Result<()> {
        let min = self.min_delay();
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
