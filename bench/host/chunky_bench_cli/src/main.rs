//! chunky-bench: the power-cut bench (HW-4, bench/README.md) from the command line. A thin shell over the
//! `chunky_bench` library; `--dry-run` runs everything on the simulated bench instead of the hardware.
//! Shell completions: `COMPLETE=bash chunky-bench` (`complete.rs`).

mod complete;

use anstream::{eprintln, print, println};
use anstyle::{AnsiColor, Style};
use anyhow::{Context, Result, bail};
use chunky_bench::dut::{Dut, read_back};
use chunky_bench::fake::{FakeBench, SimParams};
use chunky_bench::record::{self, Dumps};
use chunky_bench::run::{self, Boot, judge};
use chunky_bench::sweep::{Event, SweepFiles};
use chunky_bench::units::{parse_duration, show_duration};
use chunky_bench::{Cut, Firmware, Glitcher, Path, Power, ProbeConfig, ProbeDut, RunParams, SweepSpec, Variant};
use clap::{Args, Parser, Subcommand, ValueEnum};
use indicatif::{ProgressBar, ProgressStyle};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::time::{Duration, Instant};

const VERSION: &str = concat!(
    env!("CARGO_PKG_VERSION"),
    " (",
    env!("BENCH_GIT_SHA"),
    ", built ",
    env!("BENCH_BUILD_TIME"),
    ")"
);

const BOLD: Style = Style::new().bold();
const DIM: Style = Style::new().dimmed();
const GREEN: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Green)));
const YELLOW: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Yellow)));
const RED: Style = Style::new().fg_color(Some(anstyle::Color::Ansi(AnsiColor::Red)));

/// Power-cut bench for embassy-boot's STATE erase on the B129A: restore, cut, read back, classify, sweep.
#[derive(Parser)]
#[command(name = "chunky-bench", version = VERSION)]
struct Cli {
    #[command(flatten)]
    bench: BenchArgs,
    #[command(subcommand)]
    command: Command,
}

#[derive(Args)]
struct BenchArgs {
    /// Firmware directory: boot-{stock,guarded}.bin, app-{a,b}.bin, s0/
    #[arg(long, global = true, default_value = "bench/fw/out", env = "CHUNKY_BENCH_FW")]
    fw: PathBuf,
    /// Results directory: one directory per sweep (default bench/results; bench/host/target/dry-run with --dry-run)
    #[arg(long, global = true, env = "CHUNKY_BENCH_RESULTS")]
    results: Option<PathBuf>,
    /// Debug probe, VID:PID[:serial]
    #[arg(long, global = true, default_value = "c0de:cafe", add = complete::probes())]
    probe: String,
    /// probe-rs target
    #[arg(long, global = true, default_value = "STM32G0B1CEUx")]
    chip: String,
    /// SWD clock in kHz (Donguru: at most 1000)
    #[arg(long, global = true, default_value_t = 1000)]
    speed_khz: u32,
    /// Glitcher serial port (default: the first Xyloni found)
    #[arg(long, global = true, add = complete::glitcher_ports())]
    glitcher: Option<String>,
    /// No hardware: the simulated bench (real embassy-boot on a flash model) and stand-in firmware
    #[arg(long, global = true)]
    dry_run: bool,
    /// How the bench is wired: glitcher output on the power switch (push-pull), or on NRST (open-drain pulse)
    #[arg(long, global = true, value_enum, default_value = "power")]
    cut: CutArg,
    /// --cut reset: how long NRST is held low
    #[arg(long, global = true, default_value = "100us", value_parser = dur)]
    width: Duration,
}

#[derive(Clone, Copy, ValueEnum, PartialEq, Eq)]
enum CutArg {
    /// Power off through Donguru's Q101/Q102; every restart is a power cycle
    Power,
    /// NRST low for --width, power stays on; every restart is a reset pulse
    Reset,
}

impl BenchArgs {
    fn cut(&self) -> Cut {
        match self.cut {
            CutArg::Power => Cut::Power,
            CutArg::Reset => Cut::Reset { width: self.width },
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
enum VariantArg {
    Stock,
    Guarded,
}

impl From<VariantArg> for Variant {
    fn from(v: VariantArg) -> Variant {
        match v {
            VariantArg::Stock => Variant::Stock,
            VariantArg::Guarded => Variant::Guarded,
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
enum PathArg {
    /// B resets unconfirmed; the bootloader's revert erases STATE
    Revert,
    /// B confirms; mark_booted erases STATE
    MarkBooted,
}

impl From<PathArg> for Path {
    fn from(p: PathArg) -> Path {
        match p {
            PathArg::Revert => Path::Revert,
            PathArg::MarkBooted => Path::MarkBooted,
        }
    }
}

#[derive(Clone, Copy, ValueEnum)]
enum DumpsArg {
    /// Raw read-backs of every run
    All,
    /// Only runs that did not end on a whole image
    Bad,
    /// None
    None,
}

fn dur(s: &str) -> Result<Duration> {
    parse_duration(s)
}

/// What every run takes besides variant, path and delay.
#[derive(Args, Clone)]
struct RunArgs {
    /// PA6 rises this long before the erase, in µs (from the measurement: t_dead(max) + 2 ms)
    #[arg(long)]
    pre_trigger_us: u32,
    /// Power-off time of the cut and of every plain power cycle (clean power-on reset)
    #[arg(long, default_value = "300ms", value_parser = dur)]
    off: Duration,
    /// The bootloader waits this long first (the window to arm the glitcher)
    #[arg(long, default_value_t = 100)]
    boot_delay_ms: u32,
    /// Unconfirmed B resets after this long
    #[arg(long, default_value_t = 100)]
    reset_after_ms: u32,
    /// From arming (or a power cycle) to the read-back: long enough for swap, cut, power back, reboot
    #[arg(long, default_value = "5s", value_parser = dur)]
    settle: Duration,
    /// Plain power cycles after the cut, each read back
    #[arg(long, default_value_t = 1)]
    boots_after: u32,
    /// Also read STATE right after the cut, before the bootloader touches it (needs --boot-delay-ms >= 2000)
    #[arg(long)]
    raw_state: bool,
}

impl RunArgs {
    fn params(&self, variant: Variant, path: Path, delay: Duration, cut: Cut) -> RunParams {
        RunParams {
            variant,
            path,
            delay,
            cut,
            pre_trigger_us: self.pre_trigger_us,
            off: self.off,
            boot_delay_ms: self.boot_delay_ms,
            reset_after_ms: self.reset_after_ms,
            settle: self.settle,
            boots_after: self.boots_after,
            raw_state: self.raw_state,
        }
    }
}

#[derive(Subcommand)]
enum Command {
    /// Read the DUT back once and classify it; no power action
    Status,
    /// Cut the DUT's power for --off, then power it again
    Cycle {
        #[arg(long, default_value = "300ms", value_parser = dur)]
        off: Duration,
    },
    /// Provision (A stages B with mark_updated) and save snapshot S0 into <fw>/s0
    Snapshot {
        #[arg(long, default_value = "300ms", value_parser = dur)]
        off: Duration,
        #[arg(long, default_value = "3s", value_parser = dur)]
        settle: Duration,
    },
    /// Measure mode (bench/README.md M1-M3): PA6 high, pre-trigger, SCRATCH erase, every period
    Measure {
        #[arg(long, value_enum, default_value = "stock")]
        variant: VariantArg,
        #[arg(long, default_value_t = 0)]
        pre_trigger_us: u32,
        #[arg(long, default_value_t = 100)]
        period_ms: u32,
        /// Arm a cut this long after PA6 rises, once per capture (M2); without it the DUT just runs (M1)
        #[arg(long, value_parser = dur)]
        cut: Option<Duration>,
        /// Power-off time of each cut
        #[arg(long, default_value = "500ms", value_parser = dur)]
        off: Duration,
        /// Cuts to fire
        #[arg(long, default_value_t = 5)]
        captures: u32,
        /// Without --cut: read the firmware's erase time from the report after this long
        #[arg(long, default_value = "2s", value_parser = dur)]
        wait: Duration,
    },
    /// One run: restore S0, cut `delay` after PA6, read back, classify
    Run {
        #[arg(long, value_enum)]
        variant: VariantArg,
        #[arg(long, value_enum)]
        path: PathArg,
        /// From PA6 rising to the cut
        #[arg(long, value_parser = dur)]
        delay: Duration,
        #[command(flatten)]
        run: RunArgs,
    },
    /// A sweep into <results>/<name>/runs.csv: coarse grid, then refinement around outcome changes; resumable
    Sweep {
        /// Sweep name (an existing one is resumed)
        #[arg(long, add = complete::sweeps())]
        name: String,
        #[arg(long, value_enum, value_delimiter = ',', default_value = "stock,guarded")]
        variants: Vec<VariantArg>,
        #[arg(long, value_enum, value_delimiter = ',', default_value = "revert,mark-booted")]
        paths: Vec<PathArg>,
        /// First delay
        #[arg(long, value_parser = dur)]
        from: Duration,
        /// Last delay
        #[arg(long, value_parser = dur)]
        to: Duration,
        /// Coarse step
        #[arg(long, default_value = "100us", value_parser = dur)]
        step: Duration,
        /// Fine step between outcome changes (0: no refinement)
        #[arg(long, default_value = "10us", value_parser = dur)]
        fine: Duration,
        /// Runs per point
        #[arg(long, default_value_t = 1)]
        repeats: u32,
        #[arg(long, value_enum, default_value = "all")]
        dumps: DumpsArg,
        /// Stop after this many runs failing in a row
        #[arg(long, default_value_t = 3)]
        max_errors: u32,
        #[command(flatten)]
        run: RunArgs,
    },
}

/// The bench: real hardware or the simulation.
struct Bench {
    power: Box<dyn Power>,
    dut: Box<dyn Dut>,
    fw: Firmware,
    results: PathBuf,
    dry: bool,
}

impl BenchArgs {
    fn open(&self, with_power: bool, with_s0: bool) -> Result<Bench> {
        if self.dry_run {
            let fw = Firmware::synthetic();
            let sim = FakeBench::new(
                &fw,
                SimParams {
                    jitter: Duration::from_micros(30),
                    // A reset stops the core at once; a power cut only after the hold-up.
                    t_dead: match self.cut() {
                        Cut::Power => SimParams::default().t_dead,
                        Cut::Reset { .. } => Duration::ZERO,
                    },
                    ..SimParams::default()
                },
            );
            let results = self
                .results
                .clone()
                .unwrap_or_else(|| "bench/host/target/dry-run".into());
            return Ok(Bench {
                power: Box::new(sim.power()),
                dut: Box::new(sim.dut()),
                fw,
                results,
                dry: true,
            });
        }
        let fw = Firmware::load(&self.fw, with_s0)?;
        let power: Box<dyn Power> = if with_power {
            Box::new(Glitcher::open(self.glitcher.as_deref(), self.cut()).context("opening the glitcher")?)
        } else {
            Box::new(NoPower)
        };
        let dut = ProbeDut::new(ProbeConfig {
            probe: self.probe.clone(),
            chip: self.chip.clone(),
            speed_khz: self.speed_khz,
        });
        let results = self.results.clone().unwrap_or_else(|| "bench/results".into());
        Ok(Bench {
            power,
            dut: Box::new(dut),
            fw,
            results,
            dry: false,
        })
    }
}

/// For commands that never touch power.
struct NoPower;

impl Power for NoPower {
    fn power_cycle(&mut self, _: Duration) -> Result<()> {
        bail!("no glitcher opened")
    }
    fn arm_cut(&mut self, _: Duration, _: Duration) -> Result<()> {
        bail!("no glitcher opened")
    }
    fn fired(&mut self) -> Result<bool> {
        Ok(false)
    }
    fn disarm(&mut self) -> Result<()> {
        Ok(())
    }
}

fn style_of(outcome: &str) -> Style {
    match outcome {
        "old" | "new" => GREEN,
        "lost" => YELLOW,
        _ => RED,
    }
}

fn show_boot(n: usize, b: &Boot) {
    let rb = &b.readback;
    let st = chunky_bench::state::decode(&rb.state.data, &rb.state.unreadable);
    let s = style_of(b.outcome.name());
    let report = match rb.report() {
        Some(r) => format!(
            "report: variant {} boot {} image {:#x} app {} ecc faults {} STATE erases {} erase {} us",
            r.variant,
            record::boot_state(r.boot_state),
            r.app_image,
            record::app_stage(r.app_stage),
            r.ecc_faults,
            r.state_erases,
            r.erase_us
        ),
        None => "no report".into(),
    };
    let unread = rb.state.unreadable.len() + rb.active.unreadable.len() + rb.dfu.unreadable.len();
    println!(
        "  boot {n}  {s}{:<18}{s:#} flash {:<10} IPSR {:<2} ECCR {:#010x} (read {:#010x}) SR {:#010x}  STATE {st}{}",
        b.outcome.to_string(),
        b.flash.to_string(),
        rb.ipsr(),
        rb.eccr,
        rb.eccr_read,
        rb.sr,
        if unread > 0 {
            format!("  {RED}{unread} unreadable words{RED:#}")
        } else {
            String::new()
        }
    );
    println!("          {DIM}{report}{DIM:#}");
}

fn dry_note(b: &Bench) {
    if b.dry {
        eprintln!("{YELLOW}dry run{YELLOW:#}: simulated bench, stand-in firmware");
    }
}

fn main() -> Result<()> {
    clap_complete::CompleteEnv::with_factory(<Cli as clap::CommandFactory>::command).complete();
    let cli = Cli::parse();
    match cli.command {
        Command::Status => {
            // On reset wiring this also puts the glitcher's output into open-drain (it comes up push-pull).
            let mut b = cli.bench.open(cli.bench.cut == CutArg::Reset, false)?;
            dry_note(&b);
            if b.dry {
                // Something to look at: a provisioned DUT.
                run::provision(
                    &mut *b.power,
                    &mut *b.dut,
                    &b.fw,
                    Duration::ZERO,
                    Duration::ZERO,
                    cli.bench.cut(),
                )?;
            }
            b.dut.attach()?;
            let rb = read_back(&mut *b.dut);
            b.dut.detach();
            let boot = judge(&b.fw, rb?);
            show_boot(0, &boot);
        }
        Command::Cycle { off } => {
            let mut b = cli.bench.open(true, false)?;
            dry_note(&b);
            b.power.power_cycle(off)?;
            println!("power cycled ({} off)", show_duration(off));
        }
        Command::Snapshot { off, settle } => {
            let mut b = cli.bench.open(true, false)?;
            dry_note(&b);
            let s0 = run::provision(&mut *b.power, &mut *b.dut, &b.fw, off, settle, cli.bench.cut())?;
            if b.dry {
                println!(
                    "S0 taken on the simulated bench (not saved): STATE {}",
                    chunky_bench::state::decode(&s0.state, &[])
                );
            } else {
                Firmware::save_s0(&cli.bench.fw, &s0.state, &s0.active, &s0.dfu)?;
                println!("{GREEN}S0 saved{GREEN:#} to {}", cli.bench.fw.join("s0").display());
            }
        }
        Command::Measure {
            variant,
            pre_trigger_us,
            period_ms,
            cut,
            off,
            captures,
            wait,
        } => {
            if cli.bench.dry_run {
                bail!("measure is for the scope on the real bench; the simulation has nothing to show");
            }
            measure(
                &cli.bench,
                variant.into(),
                pre_trigger_us,
                period_ms,
                cut,
                off,
                captures,
                wait,
            )?;
        }
        Command::Run {
            variant,
            path,
            delay,
            run,
        } => {
            let mut b = cli.bench.open(true, true)?;
            dry_note(&b);
            let p = run.params(variant.into(), path.into(), delay, cli.bench.cut());
            let t = Instant::now();
            let rec = chunky_bench::run_once(&mut *b.power, &mut *b.dut, &b.fw, &p)?;
            println!(
                "{BOLD}{} {} d={}{BOLD:#}: {} pages restored, cut {}",
                p.variant,
                p.path,
                show_duration(delay),
                rec.restored,
                if rec.fired { "fired" } else { "did NOT fire" }
            );
            for (n, boot) in rec.boots.iter().enumerate() {
                show_boot(n, boot);
            }
            let s = style_of(rec.outcome.name());
            println!(
                "{BOLD}final{BOLD:#} {s}{}{s:#}  ({:.1}s)",
                rec.outcome,
                t.elapsed().as_secs_f64()
            );
        }
        Command::Sweep {
            name,
            variants,
            paths,
            from,
            to,
            step,
            fine,
            repeats,
            dumps,
            max_errors,
            run,
        } => {
            let mut b = cli.bench.open(true, true)?;
            dry_note(&b);
            let spec = SweepSpec {
                name,
                variants: variants.into_iter().map(Into::into).collect(),
                paths: paths.into_iter().map(Into::into).collect(),
                from,
                to,
                step,
                fine,
                repeats,
                base: run.params(Variant::Stock, Path::Revert, Duration::ZERO, cli.bench.cut()),
                dumps: match dumps {
                    DumpsArg::All => Dumps::All,
                    DumpsArg::Bad => Dumps::Bad,
                    DumpsArg::None => Dumps::None,
                },
                max_errors,
            };
            sweep(&mut b, &spec)?;
        }
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn measure(
    args: &BenchArgs,
    variant: Variant,
    pre_trigger_us: u32,
    period_ms: u32,
    cut: Option<Duration>,
    off: Duration,
    captures: u32,
    wait: Duration,
) -> Result<()> {
    let mut b = args.open(true, false)?;
    let n = run::measure_setup(&mut *b.dut, &b.fw, variant, pre_trigger_us, period_ms, args.cut())?;
    println!(
        "measure mode: {variant} bootloader, pre_trigger {pre_trigger_us} us, every {period_ms} ms ({n} pages written)"
    );
    b.power.power_cycle(Duration::from_millis(300))?;
    let Some(delay) = cut else {
        println!("running: PA6 high for each SCRATCH erase; capture in Scopy (trigger DIO0 rising)");
        std::thread::sleep(wait);
        b.dut.attach()?;
        let mut raw = vec![0; bench_layout::REPORT.len as usize];
        let r = b.dut.read(bench_layout::REPORT.at, &mut raw);
        b.dut.detach();
        r?;
        match bench_layout::Report::decode(&raw) {
            Some(r) => println!("firmware's last erase: {BOLD}{} us{BOLD:#}", r.erase_us),
            None => println!("{YELLOW}no report{YELLOW:#}: is the measure firmware running?"),
        }
        return Ok(());
    };
    for i in 1..=captures {
        b.power.arm_cut(delay, off)?;
        let until = Instant::now() + Duration::from_millis(u64::from(period_ms) * 3) + Duration::from_secs(2);
        while !b.power.fired()? {
            if Instant::now() > until {
                b.power.disarm()?;
                bail!("capture {i}: the cut did not fire (no PA6 edge?)");
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        println!(
            "capture {i}/{captures}: cut {} after PA6, {} off",
            show_duration(delay),
            show_duration(off)
        );
        // Power back, the DUT boots and settles into its period again.
        std::thread::sleep(off + Duration::from_millis(500));
    }
    Ok(())
}

fn sweep(b: &mut Bench, spec: &SweepSpec) -> Result<()> {
    let files = SweepFiles::new(&b.results, &spec.name);
    println!(
        "{BOLD}sweep {}{BOLD:#}: {} × {} × {}..{} step {} (fine {}) × {} → {}",
        spec.name,
        spec.variants
            .iter()
            .map(|v| v.to_string())
            .collect::<Vec<_>>()
            .join(","),
        spec.paths.iter().map(|p| p.to_string()).collect::<Vec<_>>().join(","),
        show_duration(spec.from),
        show_duration(spec.to),
        show_duration(spec.step),
        show_duration(spec.fine),
        spec.repeats,
        files.csv.display()
    );
    let pb = ProgressBar::new(0);
    pb.set_style(
        ProgressStyle::with_template("{prefix:>7} [{bar:30}] {pos}/{len} {elapsed_precise} eta {eta_precise}  {msg}")?
            .progress_chars("=> "),
    );
    let mut tally: BTreeMap<String, usize> = BTreeMap::new();
    let t = Instant::now();
    let sum = chunky_bench::run_sweep(spec, &b.results, &mut *b.power, &mut *b.dut, &b.fw, &mut |e| match e {
        Event::Planned { phase, points, skipped } => {
            pb.set_prefix(phase);
            pb.inc_length(points as u64);
            if skipped > 0 {
                pb.println(format!("{phase}: {skipped} points already in the CSV, skipped"));
            }
        }
        Event::Ran { point, outcome, detail } => {
            *tally.entry(outcome.to_string()).or_default() += 1;
            let counts: Vec<String> = tally.iter().map(|(k, v)| format!("{k} {v}")).collect();
            pb.set_message(format!(
                "{} {} {}: {detail}  [{}]",
                point.variant,
                point.path,
                show_duration(point.delay()),
                counts.join(", ")
            ));
            if !matches!(outcome, "old" | "new") {
                pb.println(format!(
                    "{} {} {} r{}: {detail}",
                    point.variant,
                    point.path,
                    show_duration(point.delay()),
                    point.repeat
                ));
            }
            pb.inc(1);
        }
        Event::Failed { point, error } => {
            pb.println(format!(
                "{} {} {}: run failed: {error:#}",
                point.variant,
                point.path,
                show_duration(point.delay())
            ));
            pb.inc(1);
        }
    });
    pb.finish_and_clear();
    let sum = sum?;
    println!(
        "{} runs in {:.0}s ({} failed, {} already done)",
        sum.ran,
        t.elapsed().as_secs_f64(),
        sum.failed,
        sum.skipped
    );
    summary_table(&files)?;
    Ok(())
}

/// Outcomes per variant and path over the whole CSV.
fn summary_table(files: &SweepFiles) -> Result<()> {
    let done = record::read_done(&files.csv)?;
    let cols = ["old", "new", "mixed", "lost", "brick", "error"];
    let mut by: BTreeMap<(Variant, Path), BTreeMap<&str, usize>> = BTreeMap::new();
    for d in &done {
        let fin = cols.iter().find(|c| **c == d.fin).copied().unwrap_or("error");
        *by.entry((d.point.variant, d.point.path))
            .or_default()
            .entry(fin)
            .or_default() += 1;
    }
    print!("{BOLD}{:<8} {:<12}", "variant", "path");
    for c in cols {
        print!("{c:>7}");
    }
    println!("{BOLD:#}");
    for ((v, p), counts) in by {
        print!("{:<8} {:<12}", v.to_string(), p.to_string());
        for c in cols {
            let n = counts.get(c).copied().unwrap_or(0);
            let s = if n == 0 { DIM } else { style_of(c) };
            print!("{s}{n:>7}{s:#}");
        }
        println!();
    }
    println!("{DIM}{}{DIM:#}", files.csv.display());
    Ok(())
}
