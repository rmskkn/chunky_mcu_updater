//! expect: the factory image does not fit
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Chip::single(Line::F1, 128 * 1024),
    Board {
        profile: Profile::Swap,
        bootloader: 20 * 1024,
        config: 0,
        chunk: 1024,
        image: ImageSize::Exact(52 * 1024),
        factory: true,
    },
);

fn main() {
    println!("{:?}", LAYOUT);
}
