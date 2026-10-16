//! The build errors of a wrong layout: every file in `tests/build_errors/` is a small program that must fail to
//! compile with the message on its first line (`//! expect: ...`), or compile (`//! builds`). A second line
//! `//! features: ...` adds chunky features.
//!
//! All cases of one feature set are the binaries of one throwaway crate, each checked on its own. The message
//! is matched as a substring, so a new compiler that words its own part of the error differently does not break
//! the test.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

struct Case {
    name: String,
    path: PathBuf,
    /// The message the build must stop with; `None` when it must build.
    expect: Option<String>,
}

fn cases() -> BTreeMap<String, Vec<Case>> {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/build_errors");
    let mut groups: BTreeMap<String, Vec<Case>> = BTreeMap::new();
    for entry in fs::read_dir(&dir).expect("tests/build_errors") {
        let path = entry.expect("a directory entry").path();
        let name = path.file_stem().expect("a file name").to_string_lossy().into_owned();
        let source = fs::read_to_string(&path).expect("a case");
        let mut lines = source.lines();
        let first = lines.next().unwrap_or_default();
        let expect = match first.strip_prefix("//! expect: ") {
            Some(message) => Some(message.to_string()),
            None => {
                assert_eq!(first, "//! builds", "{name}: the first line says what to expect");
                None
            }
        };
        let features = lines
            .next()
            .and_then(|line| line.strip_prefix("//! features: "))
            .unwrap_or_default()
            .to_string();
        groups.entry(features).or_default().push(Case { name, path, expect });
    }
    groups
}

/// The throwaway crate for the cases of one feature set.
fn crate_for(index: usize, features: &str, cases: &[Case]) -> PathBuf {
    let chunky = Path::new(env!("CARGO_MANIFEST_DIR"));
    let root = Path::new(env!("CARGO_TARGET_TMPDIR")).join(format!("build_errors_{index}"));
    fs::create_dir_all(&root).expect("a directory for the throwaway crate");
    let mut features: Vec<&str> = features.split_whitespace().collect();
    features.push("ed25519");
    let mut manifest = format!(
        "[package]\nname = \"build_errors\"\nversion = \"0.0.0\"\nedition = \"2024\"\n\n[workspace]\n\n\
         [dependencies]\nchunky = {{ path = {:?}, features = {features:?} }}\n",
        chunky
    );
    for case in cases {
        manifest += &format!("\n[[bin]]\nname = {:?}\npath = {:?}\n", case.name, case.path);
    }
    fs::write(root.join("Cargo.toml"), manifest).expect("the manifest");
    // The versions the workspace is locked to: nothing to resolve, nothing to download.
    fs::copy(chunky.join("../Cargo.lock"), root.join("Cargo.lock")).expect("the workspace lock file");
    root
}

/// Checks one case: whether it built, and what cargo said.
fn check(root: &Path, case: &Case) -> (bool, String) {
    let output = Command::new(env!("CARGO"))
        .args(["check", "--offline", "--color", "never", "--bin", &case.name])
        .current_dir(root)
        .env("CARGO_TARGET_DIR", root.join("target"))
        .env_remove("RUSTFLAGS")
        .output()
        .expect("cargo runs");
    (
        output.status.success(),
        String::from_utf8_lossy(&output.stderr).into_owned(),
    )
}

#[test]
fn wrong_layouts_stop_the_build_with_their_reason() {
    let groups = cases();
    let mut failing = 0;
    for (index, (features, cases)) in groups.iter().enumerate() {
        let root = crate_for(index, features, cases);
        for case in cases {
            let (built, said) = check(&root, case);
            match &case.expect {
                Some(message) => {
                    failing += 1;
                    assert!(!built, "{} builds, but must not", case.name);
                    assert!(
                        said.contains(message.as_str()),
                        "{} does not stop with \"{message}\":\n{said}",
                        case.name
                    );
                }
                None => assert!(built, "{} must build:\n{said}", case.name),
            }
        }
    }
    assert!(failing >= 12, "{failing} build errors tested");
}
