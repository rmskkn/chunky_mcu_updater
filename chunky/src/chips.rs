//! Chips we have a flash layout for. Anything else fails the build.
//!
//! On a firmware target exactly one `chip-*` feature must be on. `chip-custom` is the explicit way out: bring your
//! own `FlashGeometry` and check it with [`FlashGeometry::checked_on`] against the chip's erase runs.
//!
//! Offsets are from the start of the flash device (STM32: `0x0800_0000`).

use crate::geometry::{BackendKind, EraseRun, Family, FlashGeometry, Region};

#[cfg(all(
    target_os = "none",
    not(any(
        feature = "chip-stm32f103cb",
        feature = "chip-stm32g0b1re",
        feature = "chip-stm32h743zi",
        feature = "chip-rp2350",
        feature = "chip-custom"
    ))
))]
compile_error!(
    "chunky: no update backend for this chip: enable one of chip-stm32f103cb, chip-stm32g0b1re, chip-stm32h743zi, \
     chip-rp2350, or chip-custom with your own const-checked FlashGeometry"
);

#[cfg(any(
    all(feature = "chip-stm32f103cb", feature = "chip-stm32g0b1re"),
    all(feature = "chip-stm32f103cb", feature = "chip-stm32h743zi"),
    all(feature = "chip-stm32f103cb", feature = "chip-rp2350"),
    all(feature = "chip-stm32g0b1re", feature = "chip-stm32h743zi"),
    all(feature = "chip-stm32g0b1re", feature = "chip-rp2350"),
    all(feature = "chip-stm32h743zi", feature = "chip-rp2350"),
))]
compile_error!("chunky: more than one chip-* feature is enabled");

const KB: u32 = 1024;

/// STM32F103CB: 128 pages of 1 KB.
pub const STM32F103CB_FLASH: [EraseRun; 1] = [EraseRun {
    start: 0,
    len: 128 * KB,
    erase: KB,
}];

/// STM32F103CB, embassy-boot swap: 20 KB bootloader, then STATE, FLOOR, 52 KB ACTIVE, 53 KB DFU.
pub const STM32F103CB: FlashGeometry = FlashGeometry {
    family: Family::Stm32F1,
    backend: BackendKind::Swap,
    chunk: KB,
    erase: KB,
    write: 4,
    state: Region {
        offset: 20 * KB,
        len: KB,
    },
    floor: Region {
        offset: 21 * KB,
        len: KB,
    },
    active: Region {
        offset: 22 * KB,
        len: 52 * KB,
    },
    dfu: Region {
        offset: 74 * KB,
        len: 53 * KB,
    },
}
.checked_on(&STM32F103CB_FLASH);

/// STM32G0B1RE: 512 KB in 2 KB pages.
pub const STM32G0B1RE_FLASH: [EraseRun; 1] = [EraseRun {
    start: 0,
    len: 512 * KB,
    erase: 2 * KB,
}];

/// STM32G0B1RE, embassy-boot swap: 24 KB bootloader, 4 KB STATE, FLOOR, 238 KB ACTIVE, 240 KB DFU.
pub const STM32G0B1RE: FlashGeometry = FlashGeometry {
    family: Family::Stm32G0,
    backend: BackendKind::Swap,
    chunk: 2 * KB,
    erase: 2 * KB,
    write: 8,
    state: Region {
        offset: 24 * KB,
        len: 4 * KB,
    },
    floor: Region {
        offset: 28 * KB,
        len: 2 * KB,
    },
    active: Region {
        offset: 32 * KB,
        len: 238 * KB,
    },
    dfu: Region {
        offset: 270 * KB,
        len: 240 * KB,
    },
}
.checked_on(&STM32G0B1RE_FLASH);

/// STM32H743ZI: two banks of eight 128 KB sectors.
pub const STM32H743ZI_FLASH: [EraseRun; 1] = [EraseRun {
    start: 0,
    len: 2048 * KB,
    erase: 128 * KB,
}];

/// STM32H743ZI, hardware bank swap, 2 KB chunks inside 128 KB sectors. Each bank: bootloader sector, six image
/// sectors, one sector for STATE (bank 1) or FLOOR (bank 2). The bank backend (BOOT-2) fixes the final layout.
pub const STM32H743ZI: FlashGeometry = FlashGeometry {
    family: Family::Stm32H7,
    backend: BackendKind::Bank,
    chunk: 2 * KB,
    erase: 128 * KB,
    write: 32,
    state: Region {
        offset: 7 * 128 * KB,
        len: 128 * KB,
    },
    floor: Region {
        offset: 15 * 128 * KB,
        len: 128 * KB,
    },
    active: Region {
        offset: 128 * KB,
        len: 6 * 128 * KB,
    },
    dfu: Region {
        offset: 9 * 128 * KB,
        len: 6 * 128 * KB,
    },
}
.checked_on(&STM32H743ZI_FLASH);

/// RP2350 with 2 MB of QSPI NOR (RP2354A/B in-package): 4 KB sectors.
pub const RP2350_2MB_FLASH: [EraseRun; 1] = [EraseRun {
    start: 0,
    len: 2048 * KB,
    erase: 4 * KB,
}];

/// RP2350, boot ROM A/B partitions of 960 KB. The boot ROM backend (BOOT-3) fixes the final layout.
pub const RP2350_2MB: FlashGeometry = FlashGeometry {
    family: Family::Rp2350,
    backend: BackendKind::RpBootrom,
    chunk: 4 * KB,
    erase: 4 * KB,
    write: 1,
    state: Region {
        offset: 64 * KB,
        len: 4 * KB,
    },
    floor: Region {
        offset: 68 * KB,
        len: 4 * KB,
    },
    active: Region {
        offset: 128 * KB,
        len: 960 * KB,
    },
    dfu: Region {
        offset: 1088 * KB,
        len: 960 * KB,
    },
}
.checked_on(&RP2350_2MB_FLASH);

/// Every preset by name, for host tools.
pub const PRESETS: [(&str, &FlashGeometry); 4] = [
    ("stm32f103cb", &STM32F103CB),
    ("stm32g0b1re", &STM32G0B1RE),
    ("stm32h743zi", &STM32H743ZI),
    ("rp2350-2mb", &RP2350_2MB),
];

/// The geometry of the chip this firmware is built for.
#[cfg(feature = "chip-stm32f103cb")]
pub const SELECTED: FlashGeometry = STM32F103CB;
/// The geometry of the chip this firmware is built for.
#[cfg(feature = "chip-stm32g0b1re")]
pub const SELECTED: FlashGeometry = STM32G0B1RE;
/// The geometry of the chip this firmware is built for.
#[cfg(feature = "chip-stm32h743zi")]
pub const SELECTED: FlashGeometry = STM32H743ZI;
/// The geometry of the chip this firmware is built for.
#[cfg(feature = "chip-rp2350")]
pub const SELECTED: FlashGeometry = RP2350_2MB;
