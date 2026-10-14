//! Format 2 through the generated fixed-layout fast path (SW-44): what a minimal bootloader would link.
#![no_std]
#![no_main]

use chunky_format::{PlanHeader, read_header_fixed, read_step_fixed};
use chunky_format_size_bench::{keep, start};

#[unsafe(no_mangle)]
#[unsafe(link_section = ".text.start")]
extern "C" fn _start() -> ! {
    start(|rx| {
        let Ok(header) = read_header_fixed(rx) else {
            return;
        };
        keep(&header);
        let mut at = PlanHeader::ENCODED_LEN;
        while let Some(Ok(step)) = rx.get(at..).map(read_step_fixed) {
            keep(&step);
            at += step.len;
        }
    })
}
