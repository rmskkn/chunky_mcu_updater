//! expect: no part of this line has this flash size
//! The HAL says 512 KB in two banks (an STM32G0B1RE), the code says the single-bank G0 line, which ends at 128 KB.
use chunky::series::*;

mod embassy_stm32 {
    pub mod flash {
        #![allow(dead_code)]
        pub enum FlashBank {
            Bank1,
            Bank2,
            Otp,
        }
        pub struct FlashRegion {
            pub bank: FlashBank,
            pub size: u32,
            pub erase_size: u32,
            pub write_size: u32,
        }
        pub const BANK1_REGION: FlashRegion = FlashRegion {
            bank: FlashBank::Bank1,
            size: 262_144,
            erase_size: 2048,
            write_size: 8,
        };
        pub const BANK2_REGION: FlashRegion = FlashRegion {
            bank: FlashBank::Bank2,
            size: 262_144,
            erase_size: 2048,
            write_size: 8,
        };
        pub const FLASH_REGIONS: [&FlashRegion; 2] = [&BANK1_REGION, &BANK2_REGION];
    }
}

const LAYOUT: Layout = Layout::checked(
    chunky::embassy_stm32_chip!(Line::G0),
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
