//! Worked examples: parts we laid out with [`crate::series`], by name for host tools and tests.
//!
//! A firmware enables its `series-*` feature and computes its own layout from its part and its choices with
//! [`Layout::checked`], the way each preset here does (the presets themselves are laid out whatever series the
//! build selected, so that host tools have them all). `chip-custom` remains the way out for flash no
//! series rule covers: bring your own `FlashGeometry` and check it with [`FlashGeometry::checked_on`].
//!
//! Offsets are from the start of the flash device (STM32: `0x0800_0000`).

use crate::geometry::FlashGeometry;
use crate::series::{Board, Chip, ImageSize, Layout, Line, Profile};

const KB: u32 = 1024;

const fn board(profile: Profile, bootloader: u32, chunk: u32) -> Board {
    Board {
        profile,
        bootloader,
        config: 0,
        chunk,
        image: ImageSize::Max,
        factory: false,
    }
}

/// STM32F103CB (128 KB in 1 KB pages), embassy-boot swap: 20 KB bootloader, then STATE, FLOOR, 52 KB ACTIVE,
/// 53 KB DFU.
pub const STM32F103CB_LAYOUT: Layout =
    Layout::checked_for_any_series(Chip::single(Line::F1, 128 * KB), board(Profile::Swap, 20 * KB, KB));
/// Geometry of [`STM32F103CB_LAYOUT`].
pub const STM32F103CB: FlashGeometry = STM32F103CB_LAYOUT.geometry();

/// STM32F103CB with one image slot updated in place by a 16 KB golden image (BOOT-6): 90 KB for the application
/// where the swap layout has 52 KB. Layout only, the backend is to come.
pub const STM32F103CB_GOLDEN_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::single(Line::F1, 128 * KB),
    board(Profile::InPlaceGolden { golden: 16 * KB }, 20 * KB, KB),
);

/// STM32G071RB (128 KB in 2 KB pages, one bank), embassy-boot swap: 16 KB bootloader, 52 KB ACTIVE.
pub const STM32G071RB_LAYOUT: Layout =
    Layout::checked_for_any_series(Chip::single(Line::G0, 128 * KB), board(Profile::Swap, 16 * KB, 2 * KB));
/// Geometry of [`STM32G071RB_LAYOUT`].
pub const STM32G071RB: FlashGeometry = STM32G071RB_LAYOUT.geometry();

/// STM32G0B1RE (512 KB in 2 KB pages) as one bank, embassy-boot swap: 24 KB bootloader, 4 KB STATE, FLOOR,
/// 240 KB ACTIVE, 242 KB DFU.
pub const STM32G0B1RE_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::single(Line::G0B1, 512 * KB),
    board(Profile::Swap, 24 * KB, 2 * KB),
);
/// Geometry of [`STM32G0B1RE_LAYOUT`].
pub const STM32G0B1RE: FlashGeometry = STM32G0B1RE_LAYOUT.geometry();

/// STM32G0B1RE in dual-bank mode, hardware bank swap (nSWAP_BANK): per 256 KB bank a 24 KB bootloader, a
/// 230 KB image, and STATE (first bank) or FLOOR (second) in the last page.
pub const STM32G0B1RE_BANK_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::dual(Line::G0B1, 512 * KB),
    board(Profile::BankSwap, 24 * KB, 2 * KB),
);
/// Geometry of [`STM32G0B1RE_BANK_LAYOUT`].
pub const STM32G0B1RE_BANK: FlashGeometry = STM32G0B1RE_BANK_LAYOUT.geometry();

/// STM32G0B1RE, embassy-boot swap with a factory restore slot (BOOT-7): three image slots of 160 KB. The
/// factory slot is layout only, restoring from it is to come.
pub const STM32G0B1RE_FACTORY_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::single(Line::G0B1, 512 * KB),
    Board {
        factory: true,
        ..board(Profile::Swap, 24 * KB, 2 * KB)
    },
);

/// STM32H743ZI (two banks of eight 128 KB sectors), hardware bank swap (`chunky_bank`), 2 KB chunks.
///
/// Each bank: one sector for the bootloader (the same bytes in both banks, since SWAP_BANK moves the whole bank,
/// its first sector included), six image sectors (768 KB), and one more sector: STATE in bank 1, FLOOR in
/// bank 2. Offsets are as mapped with SWAP_BANK clear; with it set the banks trade addresses
/// ([`FlashGeometry::swapped`]). STATE holds 511 updates before it is erased, FLOOR counts to 4096.
pub const STM32H743ZI_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::dual(Line::H74x, 2048 * KB),
    board(Profile::BankSwap, 128 * KB, 2 * KB),
);
/// Geometry of [`STM32H743ZI_LAYOUT`].
pub const STM32H743ZI: FlashGeometry = STM32H743ZI_LAYOUT.geometry();

/// STM32H7A3ZI (two banks of 128 sectors of 8 KB), hardware bank swap: per bank a 64 KB bootloader, a 952 KB
/// image, and STATE or FLOOR in the last sector.
pub const STM32H7A3ZI_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::dual(Line::H7Ax, 2048 * KB),
    board(Profile::BankSwap, 64 * KB, 2 * KB),
);
/// Geometry of [`STM32H7A3ZI_LAYOUT`].
pub const STM32H7A3ZI: FlashGeometry = STM32H7A3ZI_LAYOUT.geometry();

/// RP2354 (RP2350 with 2 MB of QSPI NOR in the package, 4 KB sectors), boot ROM A/B: 64 KB before the slots,
/// two slots of 988 KB. The boot ROM backend (BOOT-3) fixes the final layout.
pub const RP2354_LAYOUT: Layout = Layout::checked_for_any_series(
    Chip::single(Line::Rp235x, 2048 * KB),
    board(Profile::RpBootrom, 64 * KB, 4 * KB),
);
/// Geometry of [`RP2354_LAYOUT`].
pub const RP2354: FlashGeometry = RP2354_LAYOUT.geometry();

/// Every preset that has a geometry, by name, for host tools.
pub const PRESETS: [(&str, &FlashGeometry); 7] = [
    ("stm32f103cb", &STM32F103CB),
    ("stm32g071rb", &STM32G071RB),
    ("stm32g0b1re", &STM32G0B1RE),
    ("stm32g0b1re-bank", &STM32G0B1RE_BANK),
    ("stm32h743zi", &STM32H743ZI),
    ("stm32h7a3zi", &STM32H7A3ZI),
    ("rp2354", &RP2354),
];

/// Every worked layout by name, those without a backend yet included.
pub const LAYOUTS: [(&str, &Layout); 9] = [
    ("stm32f103cb", &STM32F103CB_LAYOUT),
    ("stm32f103cb-golden", &STM32F103CB_GOLDEN_LAYOUT),
    ("stm32g071rb", &STM32G071RB_LAYOUT),
    ("stm32g0b1re", &STM32G0B1RE_LAYOUT),
    ("stm32g0b1re-bank", &STM32G0B1RE_BANK_LAYOUT),
    ("stm32g0b1re-factory", &STM32G0B1RE_FACTORY_LAYOUT),
    ("stm32h743zi", &STM32H743ZI_LAYOUT),
    ("stm32h7a3zi", &STM32H7A3ZI_LAYOUT),
    ("rp2354", &RP2354_LAYOUT),
];
