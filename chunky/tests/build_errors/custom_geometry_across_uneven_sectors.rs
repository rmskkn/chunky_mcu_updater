//! expect: a partition crosses flash with another erase size (uneven sectors)
use chunky::geometry::*;

const FLASH: [EraseRun; 2] = [
    EraseRun {
        start: 0,
        len: 65536,
        erase: 16384,
    },
    EraseRun {
        start: 65536,
        len: 983040,
        erase: 131072,
    },
];

const GEOMETRY: FlashGeometry = FlashGeometry {
    family: Family::Stm32G0,
    backend: BackendKind::Swap,
    chunk: 2048,
    erase: 16384,
    write: 8,
    state: Region { offset: 0, len: 16384 },
    floor: Region {
        offset: 16384,
        len: 16384,
    },
    active: Region {
        offset: 32768,
        len: 16384,
    },
    dfu: Region {
        offset: 49152,
        len: 32768,
    },
}
.checked_on(&FLASH);

fn main() {
    println!("{:?}", GEOMETRY);
}
