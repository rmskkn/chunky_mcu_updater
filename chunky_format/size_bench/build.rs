// link.x is found next to the manifest.
fn main() {
    println!("cargo::rustc-link-search={}", env!("CARGO_MANIFEST_DIR"));
    println!("cargo::rerun-if-changed=link.x");
}
