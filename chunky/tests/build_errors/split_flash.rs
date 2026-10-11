//! expect: the two banks are not adjacent
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Part::dual(Line::H74x, 1024 * 1024),
    Board {
        profile: Profile::Swap,
        bootloader: 128 * 1024,
        config: 0,
        chunk: 2048,
        image: ImageSize::Max,
        factory: false,
    },
);

fn main() {
    println!("{:?}", LAYOUT);
}
