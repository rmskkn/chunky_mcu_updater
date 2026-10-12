//! Sweeps: variants × paths × delays × repeats, a coarse grid first, then a fine one between every two
//! neighbouring delays whose outcomes differ. Resumable: the CSV is the only state; a point with a row (other
//! than `error`) is not run again, and the refinement is recomputed from all rows.

use crate::dut::Dut;
use crate::fw::Firmware;
use crate::power::Power;
use crate::record::{self, Done, Dumps, Log, Point};
use crate::run::{Path, RunParams, Variant, run_once};
use anyhow::{Result, bail};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::path::{Path as FsPath, PathBuf};
use std::time::Duration;

/// What to sweep.
#[derive(Clone, Debug)]
pub struct SweepSpec {
    /// Directory name under the results directory.
    pub name: String,
    pub variants: Vec<Variant>,
    pub paths: Vec<Path>,
    pub from: Duration,
    /// Inclusive when on the grid.
    pub to: Duration,
    pub step: Duration,
    /// Step of the refinement between outcome changes (zero: no refinement).
    pub fine: Duration,
    pub repeats: u32,
    /// Everything else of a run (its variant, path and delay are set per point).
    pub base: RunParams,
    pub dumps: Dumps,
    /// Stop after this many runs failing in a row (probe gone, glitcher gone).
    pub max_errors: u32,
}

impl SweepSpec {
    pub fn check(&self) -> Result<()> {
        if self.step.is_zero()
            || self.to < self.from
            || self.variants.is_empty()
            || self.paths.is_empty()
            || self.repeats == 0
        {
            bail!("empty sweep: need step > 0, to >= from, a variant, a path and repeats >= 1");
        }
        Ok(())
    }

    fn params(&self, p: &Point) -> RunParams {
        RunParams {
            variant: p.variant,
            path: p.path,
            delay: p.delay(),
            ..self.base.clone()
        }
    }

    fn points_at(&self, v: Variant, p: Path, d_ns: u64) -> impl Iterator<Item = Point> {
        (0..self.repeats).map(move |repeat| Point {
            variant: v,
            path: p,
            d_ns,
            repeat,
        })
    }

    /// The coarse grid, in run order (variant, then path, then delay, then repeat).
    pub fn coarse(&self) -> Vec<Point> {
        let (from, to, step) = (
            self.from.as_nanos() as u64,
            self.to.as_nanos() as u64,
            self.step.as_nanos() as u64,
        );
        let mut out = vec![];
        for &v in &self.variants {
            for &p in &self.paths {
                let mut d = from;
                while d <= to {
                    out.extend(self.points_at(v, p, d));
                    d += step;
                }
            }
        }
        out
    }

    /// Fine points between neighbouring delays (of the same variant and path) whose sets of outcomes differ.
    pub fn refine(&self, done: &[Done]) -> Vec<Point> {
        let fine = self.fine.as_nanos() as u64;
        if fine == 0 {
            return vec![];
        }
        let mut by: BTreeMap<(Variant, Path), BTreeMap<u64, BTreeSet<&str>>> = BTreeMap::new();
        for d in done.iter().filter(|d| d.fin != "error") {
            let p = &d.point;
            if self.variants.contains(&p.variant) && self.paths.contains(&p.path) {
                by.entry((p.variant, p.path))
                    .or_default()
                    .entry(p.d_ns)
                    .or_default()
                    .insert(&d.fin);
            }
        }
        let mut out = vec![];
        for ((v, p), ds) in by {
            let ds: Vec<_> = ds.into_iter().collect();
            for w in ds.windows(2) {
                let ((lo, a), (hi, b)) = (&w[0], &w[1]);
                if a == b {
                    continue;
                }
                let mut d = lo + fine;
                while d < *hi {
                    out.extend(self.points_at(v, p, d));
                    d += fine;
                }
            }
        }
        out
    }
}

/// Points not yet done (a row other than `error`).
pub fn todo(points: Vec<Point>, done: &[Done]) -> Vec<Point> {
    let have: HashSet<Point> = done.iter().filter(|d| d.fin != "error").map(|d| d.point).collect();
    let mut seen = HashSet::new();
    points
        .into_iter()
        .filter(|p| !have.contains(p) && seen.insert(*p))
        .collect()
}

/// Progress of a sweep, for the CLI's progress bar.
pub enum Event<'a> {
    /// `points` more runs planned in this phase (`coarse` or `refine`), `skipped` already in the CSV.
    Planned {
        phase: &'static str,
        points: usize,
        skipped: usize,
    },
    /// A run finished; `outcome` is its final class.
    Ran {
        point: &'a Point,
        outcome: &'a str,
        detail: &'a str,
    },
    /// A run failed; it gets an `error` row and is run again on resume.
    Failed { point: &'a Point, error: &'a anyhow::Error },
}

/// Where a sweep writes.
pub struct SweepFiles {
    pub dir: PathBuf,
    pub csv: PathBuf,
    pub dumps: PathBuf,
}

impl SweepFiles {
    pub fn new(results: &FsPath, name: &str) -> SweepFiles {
        let dir = results.join(name);
        SweepFiles {
            csv: dir.join("runs.csv"),
            dumps: dir.join("dumps"),
            dir,
        }
    }
}

/// Totals of a sweep (this invocation).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Summary {
    pub ran: usize,
    pub failed: usize,
    pub skipped: usize,
    /// Final classes of the runs of this invocation.
    pub outcomes: BTreeMap<String, usize>,
}

/// Runs the sweep: coarse grid, then refinement rounds until nothing new.
pub fn run_sweep(
    spec: &SweepSpec,
    results: &FsPath,
    power: &mut dyn Power,
    dut: &mut dyn Dut,
    fw: &Firmware,
    on: &mut dyn FnMut(Event),
) -> Result<Summary> {
    spec.check()?;
    let files = SweepFiles::new(results, &spec.name);
    std::fs::create_dir_all(&files.dir)?;
    let mut log = Log::open(&files.csv)?;
    let mut sum = Summary::default();
    let mut errors_in_row = 0;
    for round in 0..8 {
        let done = record::read_done(&files.csv)?;
        let (phase, all) = if round == 0 {
            ("coarse", spec.coarse())
        } else {
            ("refine", spec.refine(&done))
        };
        let n_all = all.len();
        let points = todo(all, &done);
        let skipped = n_all - points.len();
        if round == 0 {
            sum.skipped += skipped;
        }
        if round > 0 && points.is_empty() {
            break;
        }
        on(Event::Planned {
            phase,
            points: points.len(),
            skipped,
        });
        for point in &points {
            let p = spec.params(point);
            match run_once(power, dut, fw, &p) {
                Ok(rec) => {
                    errors_in_row = 0;
                    let dir = files.dumps.join(point.slug());
                    let dumped = record::write_dumps(&dir, &rec, spec.dumps)?;
                    let rel = if dumped {
                        format!("dumps/{}", point.slug())
                    } else {
                        String::new()
                    };
                    log.append(&record::row(&spec.name, point, &rec, &rel))?;
                    sum.ran += 1;
                    *sum.outcomes.entry(rec.outcome.name().to_string()).or_default() += 1;
                    on(Event::Ran {
                        point,
                        outcome: rec.outcome.name(),
                        detail: &rec.outcome.to_string(),
                    });
                }
                Err(e) => {
                    log.append(&record::error_row(&spec.name, point, &p, &e))?;
                    sum.failed += 1;
                    on(Event::Failed { point, error: &e });
                    errors_in_row += 1;
                    if errors_in_row >= spec.max_errors {
                        return Err(e.context(format!(
                            "{errors_in_row} runs failed in a row: stopping (resume runs them again)"
                        )));
                    }
                }
            }
        }
    }
    Ok(sum)
}
