//! Times as the bench takes and writes them: `12ms`, `250us`, `1.5s`, `800ns` (a unit is required, except for 0).

use anyhow::{Result, bail};
use std::time::Duration;

/// Parses a time with a unit: `ns`, `us`, `ms` or `s`; decimals allowed.
pub fn parse_duration(s: &str) -> Result<Duration> {
    let s = s.trim();
    let split = s.find(|c: char| c.is_ascii_alphabetic()).unwrap_or(s.len());
    let (num, unit) = s.split_at(split);
    let n: f64 = match num.trim().parse() {
        Ok(n) if n >= 0.0 && f64::is_finite(n) => n,
        _ => bail!("'{s}' is not a time like 12ms, 250us or 1.5s"),
    };
    let ns = match unit {
        "ns" => n,
        "us" | "µs" => n * 1e3,
        "ms" => n * 1e6,
        "s" => n * 1e9,
        "" if n == 0.0 => 0.0,
        "" => bail!("'{s}' needs a unit: ns, us, ms or s"),
        u => bail!("unknown unit '{u}' in '{s}': use ns, us, ms or s"),
    };
    if ns > u64::MAX as f64 {
        bail!("'{s}' is too long");
    }
    Ok(Duration::from_nanos(ns.round() as u64))
}

/// Short form for humans: `12.345ms`, `250us`, `1.5s`.
pub fn show_duration(d: Duration) -> String {
    let ns = d.as_nanos();
    let (v, unit) = match ns {
        n if n >= 1_000_000_000 => (n as f64 / 1e9, "s"),
        n if n >= 1_000_000 => (n as f64 / 1e6, "ms"),
        n if n >= 1_000 => (n as f64 / 1e3, "us"),
        n => (n as f64, "ns"),
    };
    let s = format!("{v:.3}");
    let s = s.trim_end_matches('0').trim_end_matches('.');
    format!("{s}{unit}")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        assert_eq!(parse_duration("12ms").unwrap(), Duration::from_millis(12));
        assert_eq!(parse_duration("12.5 ms").unwrap(), Duration::from_micros(12_500));
        assert_eq!(parse_duration("0").unwrap(), Duration::ZERO);
        assert_eq!(parse_duration("1.5s").unwrap(), Duration::from_millis(1500));
        for bad in ["", "12", "x", "-1ms", "1h", "NaNms", "1e400s"] {
            assert!(parse_duration(bad).is_err(), "{bad}");
        }
        assert_eq!(show_duration(Duration::from_micros(12_340)), "12.34ms");
        assert_eq!(show_duration(Duration::from_micros(250)), "250us");
        assert_eq!(show_duration(Duration::from_secs(2)), "2s");
    }
}
