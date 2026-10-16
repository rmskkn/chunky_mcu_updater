//! expect: the write size is not a multiple of the line's write unit
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Chip::single(Line::G0, 128 * 1024).with_write(4),
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
