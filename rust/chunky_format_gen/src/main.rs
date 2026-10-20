//! Writes and checks `chunky_format/c/chunky_format.h`, the C reader generated from the Rust types.
//!
//! The stamp next to it, `chunky_format.h.stamp`, holds the SHA-256 of the generator's inputs (the sources and
//! manifest of `chunky_format`, and the ShrinkWrap packages in `Cargo.lock`) and of the header it wrote.
//!
//! - `chunky_format_gen regen` (`make regen`): rewrites header and stamp, or does nothing when the stamp says the
//!   inputs and the header are unchanged.
//! - `chunky_format_gen check` (`make check-regen`, the pre-commit hook): fails when the header on disk is not
//!   what the types produce, or the stamp does not match the inputs or the header.

use std::fs;
use std::path::{Path, PathBuf};
use std::process::ExitCode;

use sha2::{Digest, Sha256};

const USAGE: &str = "usage: chunky_format_gen regen|check";

fn workspace() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("..")
}

fn header_path() -> PathBuf {
    workspace().join("chunky_format/c/chunky_format.h")
}

fn stamp_path() -> PathBuf {
    workspace().join("chunky_format/c/chunky_format.h.stamp")
}

fn hex(digest: &[u8]) -> String {
    digest.iter().map(|b| format!("{b:02x}")).collect()
}

/// Every file under `dir`, sorted, so the hash does not depend on the order the file system lists them in.
fn files(dir: &Path, out: &mut Vec<PathBuf>) -> std::io::Result<()> {
    for entry in fs::read_dir(dir)? {
        let path = entry?.path();
        if path.is_dir() {
            files(&path, out)?;
        } else {
            out.push(path);
        }
    }
    out.sort();
    Ok(())
}

/// The `[[package]]` entries of `Cargo.lock` for ShrinkWrap (`shrink_wrap*`): name, version and source (the git
/// commit), what decides the code its C generator writes.
fn shrink_wrap_locked(lock: &str) -> String {
    lock.split("[[package]]")
        .filter(|entry| entry.lines().any(|l| l.starts_with("name = \"shrink_wrap")))
        .flat_map(|entry| {
            entry
                .lines()
                .filter(|l| l.starts_with("name =") || l.starts_with("version =") || l.starts_with("source ="))
        })
        .collect::<Vec<_>>()
        .join("\n")
}

/// SHA-256 over the generator's inputs, each file framed by its path and length.
fn inputs_hash() -> std::io::Result<String> {
    let root = workspace();
    let crate_dir = root.join("chunky_format");
    let mut paths = vec![crate_dir.join("Cargo.toml")];
    files(&crate_dir.join("src"), &mut paths)?;
    let mut hash = Sha256::new();
    for path in &paths {
        let name = path
            .strip_prefix(&root)
            .unwrap_or(path)
            .to_string_lossy()
            .replace('\\', "/");
        let data = fs::read(path)?;
        hash.update(format!("{name} {}\n", data.len()));
        hash.update(&data);
    }
    let lock = fs::read_to_string(root.join("Cargo.lock"))?;
    let locked = shrink_wrap_locked(&lock);
    if locked.is_empty() {
        return Err(std::io::Error::other("Cargo.lock has no shrink_wrap package"));
    }
    hash.update(format!("Cargo.lock {}\n", locked.len()));
    hash.update(locked);
    Ok(hex(&hash.finalize()))
}

struct Stamp {
    inputs: String,
    header: String,
}

impl Stamp {
    fn parse(text: &str) -> Option<Stamp> {
        let field = |key: &str| {
            text.lines()
                .find_map(|l| l.strip_prefix(key).and_then(|v| v.strip_prefix(' ')))
                .map(str::to_owned)
        };
        Some(Stamp {
            inputs: field("inputs")?,
            header: field("header")?,
        })
    }

    fn render(&self) -> String {
        format!(
            "# chunky_format.h is generated from the Rust types: `make regen` rewrites it and this stamp.\n\
             inputs {}\nheader {}\n",
            self.inputs, self.header
        )
    }
}

fn generate() -> String {
    let mut header = String::new();
    chunky_format::write_c_header(&mut header).expect("writing to a String does not fail");
    header
}

fn regen() -> std::io::Result<bool> {
    let inputs = inputs_hash()?;
    let on_disk = fs::read(header_path()).ok();
    let stamp = fs::read_to_string(stamp_path()).ok().as_deref().and_then(Stamp::parse);
    if let (Some(stamp), Some(on_disk)) = (&stamp, &on_disk)
        && stamp.inputs == inputs
        && stamp.header == hex(&Sha256::digest(on_disk))
    {
        println!("chunky_format.h is up to date (inputs {})", &inputs[..12]);
        return Ok(true);
    }
    let header = generate();
    fs::write(header_path(), &header)?;
    let stamp = Stamp {
        inputs,
        header: hex(&Sha256::digest(&header)),
    };
    fs::write(stamp_path(), stamp.render())?;
    println!("chunky_format.h regenerated (inputs {})", &stamp.inputs[..12]);
    Ok(true)
}

fn check() -> std::io::Result<bool> {
    let inputs = inputs_hash()?;
    let on_disk = fs::read(header_path())?;
    let mut problems = Vec::new();
    if on_disk != generate().as_bytes() {
        problems.push("chunky_format.h is not what the types produce");
    }
    match fs::read_to_string(stamp_path()).ok().as_deref().and_then(Stamp::parse) {
        None => problems.push("chunky_format.h.stamp is missing or unreadable"),
        Some(stamp) => {
            if stamp.inputs != inputs {
                problems.push("the inputs changed since the stamp was written");
            }
            if stamp.header != hex(&Sha256::digest(&on_disk)) {
                problems.push("chunky_format.h changed since the stamp was written");
            }
        }
    }
    for problem in &problems {
        eprintln!("check-regen: {problem}");
    }
    if problems.is_empty() {
        println!("chunky_format.h is current (inputs {})", &inputs[..12]);
    } else {
        eprintln!("check-regen: run `make regen` and commit rust/chunky_format/c/");
    }
    Ok(problems.is_empty())
}

fn main() -> ExitCode {
    let result = match std::env::args().nth(1).as_deref() {
        Some("regen") => regen(),
        Some("check") => check(),
        _ => {
            eprintln!("{USAGE}");
            return ExitCode::from(2);
        }
    };
    match result {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("chunky_format_gen: {e}");
            ExitCode::FAILURE
        }
    }
}
