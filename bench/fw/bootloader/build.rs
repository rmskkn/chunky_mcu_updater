//! memory.x from the contract: the bootloader's 24 KB, RAM up to the report record.
use bench_layout::{BOOTLOADER, RAM_BASE, REPORT};

fn main() {
    let out = std::path::PathBuf::from(std::env::var("OUT_DIR").unwrap());
    let memory = format!(
        "MEMORY\n{{\n  FLASH : ORIGIN = {:#010x}, LENGTH = {}\n  RAM : ORIGIN = {:#010x}, LENGTH = {}\n}}\n",
        BOOTLOADER.at,
        BOOTLOADER.len,
        RAM_BASE,
        REPORT.at - RAM_BASE
    );
    std::fs::write(out.join("memory.x"), memory).unwrap();
    println!("cargo:rustc-link-search={}", out.display());
    println!("cargo:rerun-if-changed=build.rs");
}
