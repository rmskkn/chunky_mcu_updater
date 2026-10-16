//! `bench-fw-image <elf> <out.bin> <boot|app-a|app-b>`: the loadable segments of a bench firmware ELF as a raw
//! image at the start of its region (`bench_layout::BOOTLOADER` or `ACTIVE`). App images are padded to the whole
//! slot with `bench_layout::pad_image`, so every page of A differs from B. Fails when the binary does not fit.
//!
//! A build step of `just bench-fw`, not a user tool.

use std::process::ExitCode;

use bench_layout::{ACTIVE, BOOTLOADER, IMAGE_A, IMAGE_B, Region};
use object::Endianness;
use object::elf::PT_LOAD;
use object::read::elf::{ElfFile32, ProgramHeader};

fn convert(elf: &[u8], region: Region, pad: Option<u8>) -> Result<Vec<u8>, String> {
    let file = ElfFile32::<Endianness>::parse(elf).map_err(|e| format!("not a 32-bit ELF: {e}"))?;
    let endian = file.endian();
    let mut buf = vec![0xffu8; region.len as usize];
    let mut used = 0usize;
    for ph in file.elf_program_headers() {
        let size = ph.p_filesz(endian) as usize;
        if ph.p_type(endian) != PT_LOAD || size == 0 {
            continue;
        }
        let at = ph.p_paddr(endian);
        let data = ph
            .data(endian, elf)
            .map_err(|_| "segment outside the file".to_string())?;
        let start = at
            .checked_sub(region.at)
            .map(|o| o as usize)
            .filter(|o| o + size <= buf.len())
            .ok_or_else(|| format!("segment {at:#010x}+{size} outside {:#010x}+{}", region.at, region.len))?;
        buf[start..start + size].copy_from_slice(data);
        used = used.max(start + size);
    }
    if used == 0 {
        return Err("no loadable segment".into());
    }
    match pad {
        Some(image) => {
            if !bench_layout::pad_image(&mut buf, used, image) {
                return Err("does not fit the slot".into());
            }
        }
        None => buf.truncate(used),
    }
    eprintln!(
        "{used} bytes used of {} ({:.0} %)",
        region.len,
        100.0 * used as f64 / region.len as f64
    );
    Ok(buf)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [elf, out, kind] = args.as_slice() else {
        eprintln!("usage: bench-fw-image <elf> <out.bin> <boot|app-a|app-b>");
        return ExitCode::from(2);
    };
    let (region, pad) = match kind.as_str() {
        "boot" => (BOOTLOADER, None),
        "app-a" => (ACTIVE, Some(IMAGE_A)),
        "app-b" => (ACTIVE, Some(IMAGE_B)),
        other => {
            eprintln!("unknown kind {other:?}: boot, app-a or app-b");
            return ExitCode::from(2);
        }
    };
    let result = std::fs::read(elf)
        .map_err(|e| format!("{elf}: {e}"))
        .and_then(|bytes| convert(&bytes, region, pad))
        .and_then(|img| std::fs::write(out, img).map_err(|e| format!("{out}: {e}")));
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{elf}: {e}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn garbage_is_an_error() {
        assert!(convert(b"", ACTIVE, Some(IMAGE_A)).is_err());
        assert!(convert(&[0x7f, b'E', b'L', b'F', 1, 1, 1, 0], ACTIVE, None).is_err());
        assert!(convert(&[0u8; 4096], BOOTLOADER, None).is_err());
    }
}
