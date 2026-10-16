//! builds
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Chip::dual(Line::G0B1, 512 * 1024),
    Board {
        profile: Profile::BankSwap,
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
