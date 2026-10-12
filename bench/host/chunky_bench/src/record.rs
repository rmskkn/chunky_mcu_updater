//! The sweep's CSV (one row per run, appended and flushed as it goes) and the raw dumps next to it.

use crate::dut::flash_regs::PROGERR;
use crate::run::{Path, RunParams, RunRecord, Variant};
use crate::state;
use anyhow::{Context, Result};
use bench_layout::{AppStage, BootState};
use std::fs::{File, OpenOptions};
use std::path::{Path as FsPath, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

/// The CSV columns. Per-boot columns hold one value per boot (the one after the cut first), joined by `|`; the
/// `r_*` report fields are those of the boot after the cut.
pub const HEADER: &[&str] = &[
    "time",
    "sweep",
    "variant",
    "path",
    "d_ns",
    "repeat",
    "pre_trigger_us",
    "off_us",
    "settle_ms",
    "boots_after",
    "fired",
    "final",
    "outcome",
    "boots",
    "flash",
    "ipsr",
    "eccr",
    "eccr_read",
    "sr",
    "progerr",
    "state_magic",
    "state_progress",
    "state_written",
    "unreadable",
    "r_variant",
    "r_boot_state",
    "r_app_image",
    "r_app_stage",
    "r_ecc_faults",
    "r_ecc_last_addr",
    "r_erase_us",
    "r_state_erases",
    "restored",
    "dump",
    "error",
    "cut",
    "width_us",
];

/// What identifies a run in a sweep.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct Point {
    pub variant: Variant,
    pub path: Path,
    pub d_ns: u64,
    pub repeat: u32,
}

impl Point {
    pub fn delay(&self) -> Duration {
        Duration::from_nanos(self.d_ns)
    }
    /// Directory name of its dumps.
    pub fn slug(&self) -> String {
        format!("{}-{}-d{}-r{}", self.variant, self.path, self.d_ns, self.repeat)
    }
}

/// A row read back from an existing CSV (what resuming and refining need).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Done {
    pub point: Point,
    /// `old`, `new`, `mixed`, `lost`, `brick` or `error`.
    pub fin: String,
    /// What the boot after the cut found, beyond the outcome: STATE's magic, what `prepare_boot` returned and
    /// the STATE erases of that boot. It differs across the edge of the erase where the outcome does not.
    pub seen: String,
}

/// Seconds since the epoch as `YYYY-MM-DDTHH:MM:SSZ`.
pub fn rfc3339(t: SystemTime) -> String {
    let secs = t.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let (days, sod) = (secs.div_euclid(86_400), secs.rem_euclid(86_400));
    // Howard Hinnant's civil_from_days.
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = yoe + era * 400 + i64::from(m <= 2);
    format!(
        "{y:04}-{m:02}-{d:02}T{:02}:{:02}:{:02}Z",
        sod / 3600,
        sod % 3600 / 60,
        sod % 60
    )
}

fn join<T>(items: impl IntoIterator<Item = T>, f: impl Fn(T) -> String) -> String {
    items.into_iter().map(f).collect::<Vec<_>>().join("|")
}

pub fn boot_state(v: u8) -> String {
    let s = [
        BootState::None,
        BootState::Boot,
        BootState::Swap,
        BootState::Revert,
        BootState::DfuDetach,
        BootState::Error,
    ]
    .into_iter()
    .find(|s| *s as u8 == v);
    s.map_or_else(|| format!("{v:#x}"), |s| format!("{s:?}").to_lowercase())
}

pub fn app_stage(v: u8) -> String {
    let s = [
        AppStage::NotStarted,
        AppStage::Running,
        AppStage::Confirmed,
        AppStage::Resetting,
        AppStage::Error,
    ]
    .into_iter()
    .find(|s| *s as u8 == v);
    s.map_or_else(|| format!("{v:#x}"), |s| format!("{s:?}").to_lowercase())
}

fn base(sweep: &str, point: &Point, p: &RunParams) -> Vec<String> {
    vec![
        rfc3339(SystemTime::now()),
        sweep.into(),
        point.variant.to_string(),
        point.path.to_string(),
        point.d_ns.to_string(),
        point.repeat.to_string(),
        p.pre_trigger_us.to_string(),
        p.off.as_micros().to_string(),
        p.settle.as_millis().to_string(),
        p.boots_after.to_string(),
    ]
}

/// `cut` (power or reset) and the reset pulse width; last, so older sweeps' columns keep their places.
fn cut_columns(p: &RunParams) -> [String; 2] {
    [p.cut.name().into(), p.cut.width().as_micros().to_string()]
}

/// The row of a finished run.
pub fn row(sweep: &str, point: &Point, rec: &RunRecord, dump: &str) -> Vec<String> {
    let b = &rec.boots;
    let st: Vec<_> = b
        .iter()
        .map(|b| state::decode(&b.readback.state.data, &b.readback.state.unreadable))
        .collect();
    let first = b.first().and_then(|b| b.readback.report());
    let r = |f: &dyn Fn(&bench_layout::Report) -> String| first.as_ref().map(f).unwrap_or_default();
    let mut v = base(sweep, point, &rec.params);
    v.extend([
        rec.fired.to_string(),
        rec.outcome.name().into(),
        rec.outcome.to_string(),
        join(b, |b| b.outcome.to_string()),
        join(b, |b| b.flash.to_string()),
        join(b, |b| b.readback.ipsr().to_string()),
        join(b, |b| format!("{:#010x}", b.readback.eccr)),
        join(b, |b| format!("{:#010x}", b.readback.eccr_read)),
        join(b, |b| format!("{:#010x}", b.readback.sr)),
        join(b, |b| (b.readback.sr & PROGERR != 0).to_string()),
        join(&st, |s| s.magic.to_string()),
        join(&st, |s| {
            format!("{}{}", s.progress, if s.progress_valid { "" } else { "!" })
        }),
        join(&st, |s| s.written.to_string()),
        join(b, |b| {
            let rb = &b.readback;
            (rb.state.unreadable.len() + rb.active.unreadable.len() + rb.dfu.unreadable.len()).to_string()
        }),
        r(&|r| r.variant.to_string()),
        r(&|r| boot_state(r.boot_state)),
        r(&|r| format!("{:#x}", r.app_image)),
        r(&|r| app_stage(r.app_stage)),
        r(&|r| r.ecc_faults.to_string()),
        r(&|r| format!("{:#010x}", r.ecc_last_addr)),
        r(&|r| r.erase_us.to_string()),
        r(&|r| r.state_erases.to_string()),
        rec.restored.to_string(),
        dump.into(),
        String::new(),
    ]);
    v.extend(cut_columns(&rec.params));
    v
}

/// The row of a run that failed (probe or glitcher error): `final` is `error`, the message in `error`. Such rows
/// do not count as done, so a resumed sweep runs the point again.
pub fn error_row(sweep: &str, point: &Point, p: &RunParams, err: &anyhow::Error) -> Vec<String> {
    let mut v = base(sweep, point, p);
    v.push(String::new());
    v.push("error".into());
    v.resize(HEADER.len() - 3, String::new());
    v.push(format!("{err:#}").replace(['\n', '\r'], " "));
    v.extend(cut_columns(p));
    v
}

/// Appends rows to a CSV, writing the header first when the file is new; flushes after every row.
pub struct Log {
    w: csv::Writer<File>,
    pub path: PathBuf,
}

impl Log {
    pub fn open(path: &FsPath) -> Result<Log> {
        let new = std::fs::metadata(path).map(|m| m.len() == 0).unwrap_or(true);
        let f = OpenOptions::new()
            .create(true)
            .append(true)
            .open(path)
            .with_context(|| format!("opening {}", path.display()))?;
        let mut w = csv::WriterBuilder::new().has_headers(false).from_writer(f);
        if new {
            w.write_record(HEADER)?;
            w.flush()?;
        }
        Ok(Log {
            w,
            path: path.to_owned(),
        })
    }

    pub fn append(&mut self, row: &[String]) -> Result<()> {
        self.w.write_record(row)?;
        self.w.flush()?;
        Ok(())
    }
}

/// Reads the rows of an existing CSV (none when it does not exist). Rows that do not parse are skipped.
pub fn read_done(path: &FsPath) -> Result<Vec<Done>> {
    if !path.exists() {
        return Ok(vec![]);
    }
    let mut r = csv::ReaderBuilder::new()
        .flexible(true)
        .from_path(path)
        .with_context(|| format!("reading {}", path.display()))?;
    let h = r.headers()?.clone();
    let col = |name: &str| h.iter().position(|c| c == name);
    let (Some(v), Some(p), Some(d), Some(rep), Some(f)) =
        (col("variant"), col("path"), col("d_ns"), col("repeat"), col("final"))
    else {
        anyhow::bail!("{} is not a chunky-bench CSV (columns missing)", path.display());
    };
    let seen = [col("state_magic"), col("r_boot_state"), col("r_state_erases")];
    let mut out = vec![];
    for rec in r.records() {
        let Ok(rec) = rec else { continue };
        let get = |i: usize| rec.get(i).unwrap_or("");
        let point = (|| -> Option<Point> {
            Some(Point {
                variant: get(v).parse().ok()?,
                path: get(p).parse().ok()?,
                d_ns: get(d).parse().ok()?,
                repeat: get(rep).parse().ok()?,
            })
        })();
        if let Some(point) = point {
            out.push(Done {
                point,
                fin: get(f).to_string(),
                seen: seen
                    .iter()
                    .map(|c| c.map_or("", |i| get(i).split('|').next().unwrap_or("")))
                    .collect::<Vec<_>>()
                    .join("/"),
            });
        }
    }
    Ok(out)
}

/// What gets dumped per run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dumps {
    /// Every run.
    All,
    /// Runs that did not end on a whole image (mixed, lost, brick).
    Bad,
    None,
}

/// Writes the raw read-backs of a run into `dir` (`boot<N>-{report,state,active,dfu}.bin`); returns whether it
/// wrote anything.
pub fn write_dumps(dir: &FsPath, rec: &RunRecord, policy: Dumps) -> Result<bool> {
    let whole = matches!(rec.outcome.name(), "old" | "new");
    if policy == Dumps::None || (policy == Dumps::Bad && whole) {
        return Ok(false);
    }
    std::fs::create_dir_all(dir).with_context(|| format!("creating {}", dir.display()))?;
    for (i, b) in rec.boots.iter().enumerate() {
        let rb = &b.readback;
        for (name, data) in [
            ("report", &rb.report_raw),
            ("state", &rb.state.data),
            ("active", &rb.active.data),
            ("dfu", &rb.dfu.data),
        ] {
            std::fs::write(dir.join(format!("boot{i}-{name}.bin")), data)?;
        }
    }
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dates() {
        assert_eq!(rfc3339(UNIX_EPOCH), "1970-01-01T00:00:00Z");
        assert_eq!(
            rfc3339(UNIX_EPOCH + Duration::from_secs(1_791_808_496)),
            "2026-10-12T12:34:56Z"
        );
        assert_eq!(
            rfc3339(UNIX_EPOCH + Duration::from_secs(951_782_400)),
            "2000-02-29T00:00:00Z"
        );
    }

    #[test]
    fn garbage_csv_is_skipped_or_refused() {
        let dir = tempfile::tempdir().unwrap();
        let p = dir.path().join("x.csv");
        std::fs::write(&p, "a,b\n1,2\n").unwrap();
        assert!(read_done(&p).is_err());
        std::fs::write(&p, "variant,path,d_ns,repeat,final\nstock,revert,100,0,new\nnope,revert,1,0,new\nguarded,mark_booted,x,0,old\n\"broken\n").unwrap();
        let d = read_done(&p).unwrap();
        assert_eq!(d.len(), 1);
        assert_eq!(
            d[0].point,
            Point {
                variant: Variant::Stock,
                path: Path::Revert,
                d_ns: 100,
                repeat: 0
            }
        );
    }
}
