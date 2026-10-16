//! Puts the git SHA and the build time into `--version`.

use std::process::Command;

fn run(program: &str, args: &[&str]) -> String {
    Command::new(program)
        .args(args)
        .output()
        .ok()
        .filter(|out| out.status.success())
        .map(|out| String::from_utf8_lossy(&out.stdout).trim().to_string())
        .unwrap_or_else(|| "unknown".into())
}

fn main() {
    println!(
        "cargo:rustc-env=BENCH_GIT_SHA={}",
        run("git", &["rev-parse", "--short=10", "HEAD"])
    );
    println!(
        "cargo:rustc-env=BENCH_BUILD_TIME={}",
        run("date", &["-u", "+%Y-%m-%dT%H:%MZ"])
    );
    println!(
        "cargo:rerun-if-changed={}",
        run("git", &["rev-parse", "--git-path", "HEAD"])
    );
}
