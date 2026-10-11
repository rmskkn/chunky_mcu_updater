//! expect: erase size mismatch
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Part::single(Line::F1, 512 * 1024).with_erase(1024),
    Board {
        profile: Profile::Swap,
        bootloader: 16 * 1024,
        config: 0,
        chunk: 1024,
        image: ImageSize::Max,
        factory: false,
    },
);

fn main() {
    println!("{:?}", LAYOUT);
}
