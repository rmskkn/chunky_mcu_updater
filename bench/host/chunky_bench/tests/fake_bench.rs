//! The run loop, the sweep and the CSV, end to end on the fake bench (real embassy-boot on the flash model).

use anyhow::{Result, bail};
use chunky_bench::dut::{Dut, restore};
use chunky_bench::fake::{FakeBench, SimParams};
use chunky_bench::record::{self, Dumps, HEADER};
use chunky_bench::sweep::{Event, SweepFiles};
use chunky_bench::{Firmware, Path, RunParams, SweepSpec, Variant, run_once, run_sweep};
use std::time::Duration;

fn ms(n: u64) -> Duration {
    Duration::from_millis(n)
}

fn params(variant: Variant, path: Path, delay: Duration) -> RunParams {
    RunParams {
        variant,
        path,
        delay,
        pre_trigger_us: 12_000,
        off: ms(300),
        boot_delay_ms: 50,
        reset_after_ms: 100,
        settle: ms(4000),
        boots_after: 2,
    }
}

fn bench() -> (Firmware, FakeBench) {
    let fw = Firmware::synthetic();
    let b = FakeBench::new(&fw, SimParams::default());
    (fw, b)
}

#[test]
fn restore_writes_only_what_differs() {
    let (fw, b) = bench();
    let mut dut = b.dut();
    dut.attach().unwrap();
    let t = fw.targets(Variant::Stock, &params(Variant::Stock, Path::Revert, ms(0)).config());
    assert_eq!(restore(&mut dut, &t).unwrap(), 1 + 1 + 1 + 16 + 16);
    assert_eq!(restore(&mut dut, &t).unwrap(), 0);
    // Another variant: only the bootloader page that differs (its first).
    let g = fw.targets(
        Variant::Guarded,
        &params(Variant::Guarded, Path::Revert, ms(0)).config(),
    );
    assert_eq!(restore(&mut dut, &g).unwrap(), 1);
}

#[test]
fn cuts_before_and_after_the_erase_are_harmless() {
    let (fw, b) = bench();
    let (mut power, mut dut) = (b.power(), b.dut());
    // t_dead 8 ms, erase from 12 ms to 34 ms after PA6. mark_booted invalidates the progress before PA6 rises,
    // and embassy-boot takes invalid progress for a finished swap: a cut before its erase keeps B.
    for v in Variant::ALL {
        for (path, early, late) in [(Path::Revert, "old", "old"), (Path::MarkBooted, "new", "new")] {
            let r = run_once(&mut power, &mut dut, &fw, &params(v, path, ms(1))).unwrap();
            assert!(
                r.fired,
                "{:?}",
                r.boots
                    .iter()
                    .map(|b| (b.readback.report(), b.readback.ipsr(), &b.outcome))
                    .collect::<Vec<_>>()
            );
            assert_eq!(r.outcome.name(), early, "{v} {path} early: {:?}", r.outcome);
            assert_eq!(r.boots.len(), 3);
            let r = run_once(&mut power, &mut dut, &fw, &params(v, path, ms(40))).unwrap();
            assert_eq!(r.outcome.name(), late, "{v} {path} late: {:?}", r.outcome);
            assert!(r.boots[0].readback.report().is_some());
        }
    }
}

#[test]
fn stock_bricks_where_guarded_holds() {
    let (fw, b) = bench();
    let (mut power, mut dut) = (b.power(), b.dut());
    for path in Path::ALL {
        let mut stock_bad = 0;
        // Through the whole erase in steps shorter than one torn word (86 us).
        for d_us in (3_000..27_000).step_by(40) {
            let d = Duration::from_micros(d_us);
            let s = run_once(&mut power, &mut dut, &fw, &params(Variant::Stock, path, d)).unwrap();
            if !matches!(s.outcome.name(), "old" | "new") {
                stock_bad += 1;
                assert_eq!(s.outcome.to_string(), "brick:nmi", "{path} {d_us}");
                assert_ne!(s.boots[0].readback.eccr, 0, "the firmware's ECC hit shows in ECCR");
            }
            let g = run_once(&mut power, &mut dut, &fw, &params(Variant::Guarded, path, d)).unwrap();
            assert!(
                matches!(g.outcome.name(), "old" | "new"),
                "guarded {path} {d_us}: {}",
                g.outcome
            );
        }
        assert!(stock_bad > 0, "{path}: the model tears the magic word somewhere");
    }
}

fn spec(name: &str) -> SweepSpec {
    SweepSpec {
        name: name.into(),
        variants: Variant::ALL.to_vec(),
        paths: vec![Path::MarkBooted],
        // The model tears the magic word (stock: NMI) for d in 4.0..4.086 ms.
        from: Duration::from_micros(3_500),
        to: ms(5),
        step: Duration::from_micros(50),
        fine: Duration::from_micros(10),
        repeats: 1,
        base: params(Variant::Stock, Path::Revert, ms(0)),
        dumps: Dumps::Bad,
        max_errors: 3,
    }
}

#[test]
fn sweep_writes_csv_refines_and_resumes() {
    let (fw, b) = bench();
    let (mut power, mut dut) = (b.power(), b.dut());
    let dir = tempfile::tempdir().unwrap();
    let s = spec("t");
    let mut phases = vec![];
    let sum = run_sweep(&s, dir.path(), &mut power, &mut dut, &fw, &mut |e| {
        if let Event::Planned { phase, points, .. } = e {
            phases.push((phase, points));
        }
    })
    .unwrap();
    assert_eq!(phases[0], ("coarse", 2 * 31));
    assert!(phases.iter().any(|(p, n)| *p == "refine" && *n > 0), "{phases:?}");
    let files = SweepFiles::new(dir.path(), "t");
    let text = std::fs::read_to_string(&files.csv).unwrap();
    assert_eq!(text.lines().next().unwrap(), HEADER.join(","));
    let done = record::read_done(&files.csv).unwrap();
    assert_eq!(done.len(), sum.ran);
    assert_eq!(sum.failed, 0);
    // Every row is whole.
    let mut rd = csv::Reader::from_path(&files.csv).unwrap();
    for r in rd.records() {
        assert_eq!(r.unwrap().len(), HEADER.len());
    }
    // Stock bricks somewhere (dumped), guarded never.
    assert!(
        done.iter()
            .any(|d| d.point.variant == Variant::Stock && d.fin == "brick")
    );
    assert!(
        done.iter()
            .filter(|d| d.point.variant == Variant::Guarded)
            .all(|d| d.fin == "old" || d.fin == "new")
    );
    assert!(std::fs::read_dir(&files.dumps).unwrap().count() > 0);
    // Resume: nothing left to do.
    let again = run_sweep(&s, dir.path(), &mut power, &mut dut, &fw, &mut |_| {}).unwrap();
    assert_eq!((again.ran, again.skipped), (0, 2 * 31));
    assert_eq!(record::read_done(&files.csv).unwrap().len(), done.len());
}

/// A DUT whose probe goes away after `ok` attaches.
struct Flaky<D> {
    inner: D,
    ok: usize,
}

impl<D: Dut> Dut for Flaky<D> {
    fn attach(&mut self) -> Result<()> {
        if self.ok == 0 {
            bail!("probe gone");
        }
        self.ok -= 1;
        self.inner.attach()
    }
    fn detach(&mut self) {
        self.inner.detach()
    }
    fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<()> {
        self.inner.read(addr, buf)
    }
    fn read_u32(&mut self, addr: u32) -> Result<u32> {
        self.inner.read_u32(addr)
    }
    fn write_u32(&mut self, addr: u32, value: u32) -> Result<()> {
        self.inner.write_u32(addr, value)
    }
    fn program_pages(&mut self, pages: &[(u32, Vec<u8>)]) -> Result<()> {
        self.inner.program_pages(pages)
    }
    fn halt(&mut self) -> Result<u32> {
        self.inner.halt()
    }
}

#[test]
fn failing_runs_stop_the_sweep_and_run_again_on_resume() {
    let (fw, b) = bench();
    let mut power = b.power();
    let dir = tempfile::tempdir().unwrap();
    let mut s = spec("e");
    s.fine = Duration::ZERO;
    s.base.boots_after = 0;
    // Two attaches per run: two runs, then errors.
    let mut dut = Flaky { inner: b.dut(), ok: 4 };
    let err = run_sweep(&s, dir.path(), &mut power, &mut dut, &fw, &mut |_| {}).unwrap_err();
    assert!(format!("{err:#}").contains("3 runs failed in a row"), "{err:#}");
    let csv = SweepFiles::new(dir.path(), "e").csv;
    let done = record::read_done(&csv).unwrap();
    assert_eq!(done.iter().filter(|d| d.fin == "error").count(), 3);
    assert_eq!(done.len(), 5);
    let mut dut = b.dut();
    let sum = run_sweep(&s, dir.path(), &mut power, &mut dut, &fw, &mut |_| {}).unwrap();
    assert_eq!((sum.ran, sum.skipped), (2 * 31 - 2, 2));
}
