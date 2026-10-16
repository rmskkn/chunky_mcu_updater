//! The golden vectors in `chunky_format/` are what the generator makes (`just vectors` rewrites them), and the
//! plans in them are what a device needs.

use std::path::{Path, PathBuf};

use chunky_server::planner::PlanStep;
use chunky_server::vectors::{files, sets};

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../chunky_format")
}

#[test]
fn checked_in_vectors_are_what_the_generator_makes() {
    for (path, bytes) in files() {
        let full = root().join(&path);
        let on_disk = std::fs::read(&full).unwrap_or_else(|e| panic!("{}: {e}", full.display()));
        assert!(
            on_disk == bytes,
            "{path} differs from the generator: run `just vectors`"
        );
    }
}

#[test]
fn no_stale_vector_files() {
    let expected: Vec<String> = files().into_iter().map(|(p, _)| p).collect();
    for kind in ["signed", "unsigned"] {
        for entry in std::fs::read_dir(root().join("vectors").join(kind)).unwrap() {
            let name = format!("vectors/{kind}/{}", entry.unwrap().file_name().to_string_lossy());
            assert!(expected.contains(&name), "{name} is not generated any more: remove it");
        }
    }
}

#[test]
fn the_plans_have_every_kind_of_step() {
    for set in sets() {
        let steps = &set.plan.steps;
        assert!(
            steps.iter().any(|s| matches!(s, PlanStep::Erase { .. })),
            "{}",
            set.name
        );
        assert!(steps.iter().any(|s| matches!(s, PlanStep::Copy { .. })), "{}", set.name);
        assert!(
            steps.iter().any(|s| matches!(s, PlanStep::Write { .. })),
            "{}",
            set.name
        );
    }
}
