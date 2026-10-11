//! expect: the image slot does not fit in the flash
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Part::single(Line::F1, 128 * 1024),
    Board {
        profile: Profile::Swap,
        bootloader: 20 * 1024,
        config: 0,
        chunk: 1024,
        image: ImageSize::Exact(60 * 1024),
        factory: false,
    },
);

fn main() {
    println!("{:?}", LAYOUT);
}
