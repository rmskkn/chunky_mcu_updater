//! The [`Report`] record at `bench_layout::REPORT` (RAM the linker leaves out, so the runtime never touches it).

use bench_layout::{REPORT, Report};

fn word_ptr(i: usize) -> *mut u32 {
    (REPORT.at as usize + i * 4) as *mut u32
}

/// The current record, [`Report::EMPTY`] when there is none.
pub fn read() -> Report {
    let mut bytes = [0u8; Report::BYTES];
    for i in 0..Report::WORDS {
        // SAFETY: REPORT is RAM inside the G0B1's SRAM, outside the linker's RAM region, word aligned.
        let w = unsafe { word_ptr(i).read_volatile() };
        bytes[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    Report::decode(&bytes).unwrap_or(Report::EMPTY)
}

/// Writes the whole record.
pub fn write(r: &Report) {
    let bytes = r.encode();
    for i in 0..Report::WORDS {
        let w = u32::from_le_bytes([bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], bytes[i * 4 + 3]]);
        // SAFETY: as in `read`.
        unsafe { word_ptr(i).write_volatile(w) };
    }
}

/// Read, change, write. Not called from the NMI, which only touches the ECC words (see [`ecc_fault`]).
pub fn update(f: impl FnOnce(&mut Report)) {
    let mut r = read();
    f(&mut r);
    write(&r);
}

/// Counts an ECC fault at `addr` in place (words 2 and 3 of the encoding): safe to call from the NMI.
pub(crate) fn ecc_fault(addr: u32) {
    // SAFETY: as in `read`. Word 0 is the magic; without it the counts are dropped (nothing reset it yet).
    unsafe {
        if word_ptr(0).read_volatile() == Report::MAGIC {
            let n = word_ptr(2).read_volatile();
            word_ptr(2).write_volatile(n.wrapping_add(1));
            word_ptr(3).write_volatile(addr);
        }
    }
}

/// The variant the bootloader recorded this boot (the application picks its STATE wrapper by it).
pub fn variant() -> u8 {
    read().variant
}
