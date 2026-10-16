//! One image per reader (`src/bin`), each reading a plan from a buffer: the header, then every step with its data.
//! What is read goes through [black_box], so nothing is folded away. `just size-bench` builds them and prints the
//! sizes; `empty` is the floor.
#![no_std]

use core::hint::black_box;

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

static RX: [u8; 2048] = [0; 2048];

/// Runs `read` on the buffer forever.
pub fn start(read: fn(&[u8])) -> ! {
    loop {
        read(black_box(&RX[..]));
    }
}

pub use core::hint::black_box as keep;
