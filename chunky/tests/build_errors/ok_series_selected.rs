//! builds
//! features: series-stm32g0
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Part::single(Line::G0, 128 * 1024),
    Board {
        profile: Profile::Swap,
        bootloader: 16 * 1024,
        config: 0,
        chunk: 2048,
        image: ImageSize::Max,
        factory: false,
    },
);

fn main() {
    println!("{:?}", LAYOUT);
}
