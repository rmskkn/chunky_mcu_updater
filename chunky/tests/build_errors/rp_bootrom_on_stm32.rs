//! expect: the RP boot ROM profile needs series RP235x
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Chip::single(Line::G0, 128 * 1024),
    Board {
        profile: Profile::RpBootrom,
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
