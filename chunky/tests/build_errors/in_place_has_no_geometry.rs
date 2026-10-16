//! expect: the in-place profile has no DFU and no backend yet (BOOT-6)
use chunky::chips::STM32F103CB_GOLDEN_LAYOUT;
use chunky::geometry::FlashGeometry;

const GEOMETRY: FlashGeometry = STM32F103CB_GOLDEN_LAYOUT.geometry();

fn main() {
    println!("{:?}", GEOMETRY);
}
