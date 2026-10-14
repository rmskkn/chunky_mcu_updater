//! No reader: the floor of the other images.
#![no_std]
#![no_main]

use chunky_format_size_bench::{keep, start};

#[unsafe(no_mangle)]
#[unsafe(link_section = ".text.start")]
extern "C" fn _start() -> ! {
    start(|rx| {
        keep(rx.first());
    })
}
