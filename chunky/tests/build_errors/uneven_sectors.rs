//! expect: the flash has sectors of several sizes (uneven sectors)
use chunky::series::*;

const fn run(len: u32, erase: u32) -> BankRun {
    BankRun {
        bank: 1,
        len,
        erase,
        write: 4,
    }
}

// An STM32F4 as its HAL lists it: four sectors of 16 KB, one of 64 KB, seven of 128 KB.
const PART: Chip = Chip::checked_from_regions(
    Line::F1,
    &[run(64 * 1024, 16 * 1024), run(64 * 1024, 64 * 1024), run(896 * 1024, 128 * 1024)],
);

fn main() {
    println!("{:?}", PART);
}
