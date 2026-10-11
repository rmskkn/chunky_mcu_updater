//! expect: this part does not exist in this bank mode
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Part::single(Line::H74x, 2048 * 1024),
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
