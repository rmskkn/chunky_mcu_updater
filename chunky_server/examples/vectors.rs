//! Rewrites the golden vectors (`chunky_format/vectors/`, `chunky_format/c/golden.h`): `just vectors`.

use std::path::Path;

fn main() -> std::io::Result<()> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../chunky_format");
    for (path, bytes) in chunky_server::vectors::files() {
        let path = root.join(path);
        std::fs::create_dir_all(path.parent().expect("a file path"))?;
        std::fs::write(&path, bytes)?;
        println!("wrote {}", path.display());
    }
    Ok(())
}
