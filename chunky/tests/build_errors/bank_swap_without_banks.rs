//! expect: bank swap on a line without hardware bank swap
use chunky::series::*;

const LAYOUT: Layout = Layout::checked(
    Chip::single(Line::G0, 128 * 1024),
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
