//! Shell completions, produced by the binary itself when the shell calls it with `COMPLETE=<shell>` set
//! (clap_complete's dynamic completion), so they complete live values: probes plugged in, glitcher ports, sweeps
//! already in the results directory. Install once:
//!
//! ```sh
//! echo 'source <(COMPLETE=bash chunky-bench)' >> ~/.bashrc
//! echo 'source <(COMPLETE=zsh chunky-bench)' >> ~/.zshrc
//! echo 'COMPLETE=fish chunky-bench | source' >> ~/.config/fish/config.fish
//! ```

use clap_complete::CompletionCandidate;
use clap_complete::engine::ArgValueCompleter;
use std::ffi::OsStr;

/// Debug probes plugged in now, `VID:PID[:serial]`, with their name.
pub fn probes() -> ArgValueCompleter {
    ArgValueCompleter::new(|cur: &OsStr| {
        let cur = cur.to_string_lossy().to_lowercase();
        chunky_bench::probe::list()
            .into_iter()
            .filter(|(sel, _)| sel.starts_with(&cur))
            .map(|(sel, name)| CompletionCandidate::new(sel).help(Some(name.into())))
            .collect()
    })
}

/// Serial ports of Xyloni boards (the glitcher's UART is the FPGA UART).
pub fn glitcher_ports() -> ArgValueCompleter {
    ArgValueCompleter::new(|cur: &OsStr| {
        let cur = cur.to_string_lossy();
        chunky_bench::power::ports()
            .into_iter()
            .filter(|(path, _)| path.starts_with(cur.as_ref()))
            .map(|(path, what)| CompletionCandidate::new(path).help(Some(what.into())))
            .collect()
    })
}

/// Sweeps in the results directory (`bench/results`, or `CHUNKY_BENCH_RESULTS`), with their row count, so a
/// sweep is resumed by name.
pub fn sweeps() -> ArgValueCompleter {
    ArgValueCompleter::new(|cur: &OsStr| {
        let cur = cur.to_string_lossy();
        let results = std::env::var("CHUNKY_BENCH_RESULTS").unwrap_or_else(|_| "bench/results".into());
        let Ok(dir) = std::fs::read_dir(&results) else {
            return vec![];
        };
        let mut out: Vec<_> = dir
            .flatten()
            .filter_map(|e| {
                let name = e.file_name().to_string_lossy().to_string();
                let csv = e.path().join("runs.csv");
                let rows = std::fs::read_to_string(&csv).ok()?.lines().count().saturating_sub(1);
                name.starts_with(cur.as_ref())
                    .then(|| CompletionCandidate::new(&name).help(Some(format!("{rows} runs, resume").into())))
            })
            .collect();
        out.sort_by(|a, b| a.get_value().cmp(b.get_value()));
        out
    })
}
