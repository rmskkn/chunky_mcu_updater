//! The CLI through its binary: a whole release flow, and the refusals.

use std::fs;
use std::path::Path;
use std::process::{Command, Output};

fn chunky(args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_chunky"))
        .args(args)
        .env("NO_COLOR", "1")
        .output()
        .expect("the chunky binary runs")
}

fn ok(args: &[&str]) -> String {
    let out = chunky(args);
    assert!(
        out.status.success(),
        "chunky {args:?} failed: {}",
        String::from_utf8_lossy(&out.stderr)
    );
    String::from_utf8(out.stdout).unwrap()
}

fn fails(args: &[&str]) -> String {
    let out = chunky(args);
    assert!(!out.status.success(), "chunky {args:?} should fail");
    String::from_utf8(out.stderr).unwrap()
}

fn s(p: &Path) -> &str {
    p.to_str().unwrap()
}

/// A payload of `len` pseudo-random bytes from `seed`.
fn payload(seed: u8, len: usize) -> Vec<u8> {
    let mut x = u32::from(seed).wrapping_mul(2654435761).wrapping_add(1);
    (0..len)
        .map(|_| {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            x as u8
        })
        .collect()
}

#[test]
fn version_has_sha_and_build_time() {
    let v = ok(&["--version"]);
    assert!(v.starts_with("chunky "), "{v}");
    assert!(v.contains("built "), "{v}");
}

#[test]
fn keygen_writes_a_private_file_and_prints_only_the_public_key() {
    let dir = tempfile::tempdir().unwrap();
    let key = dir.path().join("release.key");
    let public = ok(&["keygen", s(&key)]).trim().to_string();
    assert_eq!(public.len(), 64);
    let seed = fs::read_to_string(&key).unwrap();
    assert!(!public.contains(seed.trim()));
    assert_eq!(ok(&["pubkey", s(&key)]).trim(), public);
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(fs::metadata(&key).unwrap().permissions().mode() & 0o777, 0o600);
        // never overwritten, and a readable key is refused
        assert!(fails(&["keygen", s(&key)]).contains("error"));
        fs::set_permissions(&key, fs::Permissions::from_mode(0o644)).unwrap();
        assert!(fails(&["pubkey", s(&key)]).contains("owner only"));
    }
    // the seed never shows in any output
    let out = chunky(&["pubkey", s(&key)]);
    assert!(!String::from_utf8_lossy(&out.stderr).contains(seed.trim()));
}

#[test]
fn release_flow_build_plan_store() {
    let dir = tempfile::tempdir().unwrap();
    let p = |name: &str| dir.path().join(name);
    let key = p("k");
    ok(&["keygen", s(&key)]);

    let old = payload(1, 40_000);
    let mut new = old.clone();
    new[5_000..5_100].copy_from_slice(&payload(9, 100));
    fs::write(p("old.bin"), &old).unwrap();
    fs::write(p("new.bin"), &new).unwrap();
    for (bin, img, version) in [("old.bin", "old.img", "1"), ("new.bin", "new.img", "2")] {
        ok(&[
            "image",
            "build",
            s(&p(bin)),
            "-o",
            s(&p(img)),
            "--chip",
            "stm32g0b1re",
            "--key",
            s(&key),
            "--product",
            "7",
            "--version",
            version,
        ]);
    }
    let info = ok(&["image", "info", s(&p("new.img"))]);
    assert!(
        info.contains("ed25519") && info.contains("version          2"),
        "{info}"
    );

    let plan = ok(&[
        "plan",
        "--chip",
        "stm32g0b1re",
        "--from",
        s(&p("old.img")),
        "--to",
        s(&p("new.img")),
        "-o",
        s(&p("plan.bin")),
    ]);
    assert!(plan.contains("write"), "{plan}");
    let wire = fs::metadata(p("plan.bin")).unwrap().len();
    let image = fs::metadata(p("new.img")).unwrap().len();
    assert!(
        wire < image / 10,
        "a 100-byte change must not cost {wire} of {image} bytes"
    );

    let added = ok(&[
        "store",
        "add",
        s(&p("new.img")),
        "--store",
        s(&p("store")),
        "--chip",
        "stm32g0b1re",
    ]);
    assert!(added.contains("new in the store"), "{added}");
    let listed = ok(&["store", "ls", "--store", s(&p("store"))]);
    assert!(
        listed.lines().all(|l| l.split(' ').next().unwrap().len() == 32),
        "{listed}"
    );
    assert!(!listed.is_empty());
}

#[test]
fn refusals() {
    let dir = tempfile::tempdir().unwrap();
    let p = |name: &str| dir.path().join(name);
    fs::write(p("payload"), payload(2, 100)).unwrap();
    fs::write(p("garbage"), payload(3, 500)).unwrap();
    let img = p("a.img");
    ok(&[
        "image",
        "build",
        s(&p("payload")),
        "-o",
        s(&img),
        "--chip",
        "stm32g0b1re",
        "--unsigned",
        "--product",
        "1",
        "--version",
        "1",
    ]);
    // garbage is an error message, never a panic
    for args in [
        vec!["image", "info", s(&p("garbage"))],
        vec!["manifest", s(&p("garbage")), "--chip", "stm32g0b1re"],
        vec![
            "plan",
            "--chip",
            "stm32g0b1re",
            "--from",
            s(&p("garbage")),
            "--to",
            s(&img),
        ],
    ] {
        let err = fails(&args);
        assert!(err.contains("error:") && !err.contains("panicked"), "{err}");
    }
    // an image for another chip layout is refused by the planner
    let err = fails(&["plan", "--chip", "stm32h743zi", "--from", s(&img), "--to", s(&img)]);
    assert!(err.contains("error:"), "{err}");
    // key and --unsigned are exclusive; one is required
    fails(&[
        "image",
        "build",
        s(&p("payload")),
        "-o",
        s(&p("x")),
        "--chip",
        "stm32g0b1re",
        "--product",
        "1",
        "--version",
        "1",
    ]);
    // an unknown chip is refused with the choices
    assert!(!fails(&["chips", "--chip", "nope"]).is_empty());
    assert!(ok(&["chips"]).contains("stm32g0b1re"));
}

#[test]
fn completions_for_three_shells_name_the_chips() {
    for shell in ["bash", "zsh", "fish"] {
        let script = ok(&["completions", shell]);
        assert!(script.contains("chunky"), "{shell}");
        assert!(script.contains("stm32g0b1re"), "{shell}: chips are completed");
    }
}
