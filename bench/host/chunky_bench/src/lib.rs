//! chunky_bench: the power-cut bench harness (HW-4, `bench/README.md`). It restores the B129A to snapshot S0,
//! cuts its power a swept delay after the firmware's PA6 trigger (embassy-boot's STATE erase), reads back what is
//! left over SWD and classifies it with embassy_torn's page classifier: `old`, `new`, `mixed`, `lost`, `brick`.
//!
//! - [`Power`]: the power switch ([`Glitcher`], the Xyloni glitcher's push-pull `od_out`).
//! - [`Dut`]: SWD access ([`ProbeDut`], probe-rs over Donguru's CMSIS-DAP v2).
//! - [`fake`]: both on a model running the real embassy-boot, for tests and dry runs.
//! - [`run_once`], [`run_sweep`]: one run, and a resumable sweep into a CSV with raw dumps.
//!
//! `chunky-bench` (crate `chunky_bench_cli`) is a thin CLI over this.

pub mod dut;
pub mod fake;
pub mod fw;
pub mod power;
pub mod probe;
pub mod record;
pub mod run;
pub mod state;
pub mod sweep;
pub mod units;

pub use dut::{Dut, Readback, read_back, restore};
pub use fw::Firmware;
pub use power::{Cut, Glitcher, Power};
pub use probe::{ProbeConfig, ProbeDut};
pub use run::{Outcome, Path, RunParams, RunRecord, Variant, run_once};
pub use sweep::{SweepSpec, run_sweep};
