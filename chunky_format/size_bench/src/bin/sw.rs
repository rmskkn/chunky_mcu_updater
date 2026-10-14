//! Format 2 through shrink_wrap's `BufReader`: what an app or a bootloader with WireWeaver would link.
#![no_std]
#![no_main]

use chunky_format::{PlanHeader, read_header_sw, read_step_sw};
use chunky_format_size_bench::{keep, start};

#[unsafe(no_mangle)]
#[unsafe(link_section = ".text.start")]
extern "C" fn _start() -> ! {
    start(|rx| {
        let Ok(header) = read_header_sw(rx) else {
            return;
        };
        keep(&header);
        let mut at = PlanHeader::ENCODED_LEN;
        while let Some(Ok(step)) = rx.get(at..).map(read_step_sw) {
            keep(&step);
            at += step.len;
        }
    })
}
