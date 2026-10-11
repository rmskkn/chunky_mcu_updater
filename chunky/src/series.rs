//! Flash layouts per MCU series: the series knows its flash, the application says what part and what profile.
//!
//! A `series-*` feature picks the series a firmware is built for. What is fixed for the series lives here
//! ([`Line::facts`]: write unit and rule, ECC fault, how banks are switched; the erase size per sub-family, density
//! and bank mode). The application gives what varies: the [`Part`] (line, flash size, bank mode) and its
//! [`Board`] choices (profile, bootloader and config sizes, chunk, image size, factory slot). [`Layout::checked`]
//! computes the partition table in a `const`, and every inconsistency stops the build with its reason.
//!
//! ```
//! use chunky::series::*;
//! // A Nucleo G071RB: 128 KB, single bank, embassy-boot A/B swap behind a 16 KB bootloader.
//! const LAYOUT: Layout = Layout::checked(
//!     Part::single(Line::G0, 128 * 1024),
//!     Board {
//!         profile: Profile::Swap,
//!         bootloader: 16 * 1024,
//!         config: 2 * 1024,
//!         chunk: 2048,
//!         image: ImageSize::Max,
//!         factory: false,
//!     },
//! );
//! assert_eq!(LAYOUT.active.len, 52 * 1024);
//! assert_eq!(LAYOUT.geometry().chunks(), 26);
//! ```
//!
//! The G071 has one bank, so asking for hardware bank swap stops the build:
//!
//! ```compile_fail
//! use chunky::series::*;
//! const LAYOUT: Layout = Layout::checked(
//!     Part::single(Line::G0, 128 * 1024),
//!     Board {
//!         profile: Profile::BankSwap,
//!         bootloader: 16 * 1024,
//!         config: 0,
//!         chunk: 2048,
//!         image: ImageSize::Max,
//!         factory: false,
//!     },
//! );
//! ```
//!
//! Offsets are from the start of the flash device (STM32: `0x0800_0000`). Where the application uses
//! embassy-stm32, [`embassy_stm32_part!`](crate::embassy_stm32_part) reads flash size, erase size, write size and
//! bank layout from the HAL's generated metadata instead.
//!
//! # How the flash is divided
//!
//! - **Swap** (embassy-boot): bootloader, STATE, FLOOR, ACTIVE, DFU (one erase unit longer), then the factory
//!   slot, spare flash, and the config region at the very end.
//! - **Bank swap**: each bank holds the bootloader, the image, the factory slot and, in its last erase unit, STATE
//!   (first bank) or FLOOR (second bank). The config region sits before STATE in the first bank; the same span of
//!   the second bank stays free. Offsets are as mapped with the swap bit clear.
//! - **RP boot ROM**: like swap, with two slots of the same size and one erase unit of STATE.
//! - **In place with a golden image** (BOOT-6, backend to come): bootloader, GOLDEN, STATE, FLOOR, ACTIVE. There
//!   is no DFU, so [`Layout::geometry`] refuses it until the backend exists.
//! - **Factory slot** (BOOT-7, backend to come): one more image-sized region with any profile.

use crate::geometry::{self, BackendKind, EraseRun, Family, FlashGeometry, GeometryError, Region};

const KB: u32 = 1024;
const MB: u32 = 1024 * KB;

/// An MCU series: what a `series-*` feature selects.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Series {
    /// STM32F1 (feature `series-stm32f1`).
    Stm32F1 = 1,
    /// STM32G0 (feature `series-stm32g0`).
    Stm32G0 = 2,
    /// STM32G4 (feature `series-stm32g4`).
    Stm32G4 = 3,
    /// STM32L4 and L4+ (feature `series-stm32l4`).
    Stm32L4 = 4,
    /// STM32H7 (feature `series-stm32h7`).
    Stm32H7 = 5,
    /// RP2350 and RP2354 (feature `series-rp235x`).
    Rp235x = 6,
}

const SELECTED_COUNT: usize = cfg!(feature = "series-stm32f1") as usize
    + cfg!(feature = "series-stm32g0") as usize
    + cfg!(feature = "series-stm32g4") as usize
    + cfg!(feature = "series-stm32l4") as usize
    + cfg!(feature = "series-stm32h7") as usize
    + cfg!(feature = "series-rp235x") as usize;

#[cfg(all(
    target_os = "none",
    not(any(
        feature = "series-stm32f1",
        feature = "series-stm32g0",
        feature = "series-stm32g4",
        feature = "series-stm32l4",
        feature = "series-stm32h7",
        feature = "series-rp235x",
        feature = "chip-custom"
    ))
))]
compile_error!(
    "chunky: no flash rules for this chip: enable one of series-stm32f1, series-stm32g0, series-stm32g4, \
     series-stm32l4, series-stm32h7, series-rp235x, or chip-custom with your own const-checked FlashGeometry"
);

#[cfg(target_os = "none")]
const _: () = assert!(SELECTED_COUNT <= 1, "chunky: more than one series-* feature is enabled");

/// The series this build is for: the one `series-*` feature that is on. `None` on a host build that enables
/// none (or several), where every series can be laid out.
pub const SELECTED: Option<Series> = if SELECTED_COUNT != 1 {
    None
} else if cfg!(feature = "series-stm32f1") {
    Some(Series::Stm32F1)
} else if cfg!(feature = "series-stm32g0") {
    Some(Series::Stm32G0)
} else if cfg!(feature = "series-stm32g4") {
    Some(Series::Stm32G4)
} else if cfg!(feature = "series-stm32l4") {
    Some(Series::Stm32L4)
} else if cfg!(feature = "series-stm32h7") {
    Some(Series::Stm32H7)
} else {
    Some(Series::Rp235x)
};

/// A sub-family inside a series: the parts that share erase size rules and bank options.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Line {
    /// STM32F100/F101/F102/F103: 1 KB pages up to 128 KB (low and medium density), 2 KB pages from 256 KB
    /// (high and XL density). XL parts have two banks one after the other and no way to swap them.
    F1,
    /// STM32F105/F107 (connectivity line): 2 KB pages at every size.
    F1Connectivity,
    /// STM32G03x to G08x: 2 KB pages, one bank.
    G0,
    /// STM32G0B0/G0B1/G0C1: 2 KB pages, one or two banks from 256 KB, nSWAP_BANK option bit.
    G0B1,
    /// STM32G43x/G44x (category 2): 2 KB pages, one bank.
    G4Cat2,
    /// STM32G47x/G48x (category 3): two banks of 2 KB pages (the second at 256 KB), or one bank of 4 KB pages.
    G4Cat3,
    /// STM32G49x/G4Ax (category 4): 2 KB pages, one bank.
    G4Cat4,
    /// STM32L41x to L46x: 2 KB pages, one bank.
    L4,
    /// STM32L47x/L48x/L49x/L4Ax: 2 KB pages, two banks.
    L4Dual,
    /// STM32L4P/L4Q/L4R/L4S (L4+): two banks of 4 KB pages, or one bank of 8 KB pages.
    L4Plus,
    /// STM32H72x/H73x: 128 KB sectors, one bank.
    H72x,
    /// STM32H74x/H75x: 128 KB sectors; two banks 1 MB apart, or the single sector of the H750.
    H74x,
    /// STM32H7A3/H7B3/H7B0: 8 KB sectors, 128-bit flash words; two banks 1 MB apart, or the 128 KB of the H7B0.
    H7Ax,
    /// RP2350 with external QSPI NOR, RP2354 with 2 MB in the package: 4 KB sectors.
    Rp235x,
}

/// What a second write to a programmed unit does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteRule {
    /// A unit is written once after an erase; all zeros may be written over anything (F1 half-words).
    OnceOrZero,
    /// The same with ECC per unit: a torn write faults when read (G0, G4, L4).
    EccOnceOrZero,
    /// A unit is written once, with ECC; nothing may be written over it (H7).
    EccOnce,
    /// Any bit can go from 1 to 0 at any time (QSPI NOR).
    BitsToZero,
}

/// What the core does when a read hits a word with an uncorrectable ECC error.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EccFault {
    /// The flash has no ECC: a torn word reads as some value.
    None,
    /// NMI (G0, G4, L4).
    Nmi,
    /// Bus fault (H7).
    BusFault,
}

/// How a dual-bank part picks the bank it boots from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BankSelect {
    /// It cannot: one bank, or two that always sit at the same addresses.
    None,
    /// A SWAP_BANK option bit trades the addresses of the banks at reset (H7; G0B1/G0C1 as nSWAP_BANK).
    SwapBank,
    /// The BFB2 option bit: the system bootloader maps the second bank first when it holds an image (G4, L4).
    /// Driving it is HW-2.
    Bfb2,
}

/// What is fixed for a line.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Facts {
    /// The series the line belongs to.
    pub series: Series,
    /// Write-rule family of the geometry.
    pub family: Family,
    /// The unit the write rule applies to, in bytes.
    pub unit: u32,
    /// The write unit a layout uses unless the part says otherwise: what embassy's flash driver writes at once
    /// (F1: two half-words, H7A3: two flash words).
    pub write: u32,
    /// The write rule.
    pub rule: WriteRule,
    /// What an uncorrectable ECC error raises.
    pub ecc: EccFault,
    /// Hardware bank swap, where the line has two banks.
    pub bank_select: BankSelect,
}

impl Line {
    /// The series of this line.
    pub const fn series(self) -> Series {
        self.facts().series
    }

    /// What is fixed for this line.
    pub const fn facts(self) -> Facts {
        const fn facts(
            series: Series,
            family: Family,
            unit: u32,
            write: u32,
            rule: WriteRule,
            ecc: EccFault,
            bank_select: BankSelect,
        ) -> Facts {
            Facts {
                series,
                family,
                unit,
                write,
                rule,
                ecc,
                bank_select,
            }
        }
        use BankSelect as B;
        use EccFault as E;
        use WriteRule as W;
        match self {
            Line::F1 | Line::F1Connectivity => {
                facts(Series::Stm32F1, Family::Stm32F1, 2, 4, W::OnceOrZero, E::None, B::None)
            }
            Line::G0 => facts(
                Series::Stm32G0,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::None,
            ),
            Line::G0B1 => facts(
                Series::Stm32G0,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::SwapBank,
            ),
            Line::G4Cat2 | Line::G4Cat4 => facts(
                Series::Stm32G4,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::None,
            ),
            Line::G4Cat3 => facts(
                Series::Stm32G4,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::Bfb2,
            ),
            Line::L4 => facts(
                Series::Stm32L4,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::None,
            ),
            Line::L4Dual | Line::L4Plus => facts(
                Series::Stm32L4,
                Family::Stm32G0,
                8,
                8,
                W::EccOnceOrZero,
                E::Nmi,
                B::Bfb2,
            ),
            Line::H72x => facts(
                Series::Stm32H7,
                Family::Stm32H7,
                32,
                32,
                W::EccOnce,
                E::BusFault,
                B::None,
            ),
            Line::H74x => facts(
                Series::Stm32H7,
                Family::Stm32H7,
                32,
                32,
                W::EccOnce,
                E::BusFault,
                B::SwapBank,
            ),
            Line::H7Ax => facts(
                Series::Stm32H7,
                Family::Stm32H7,
                16,
                32,
                W::EccOnce,
                E::BusFault,
                B::SwapBank,
            ),
            Line::Rp235x => facts(Series::Rp235x, Family::Rp2350, 1, 1, W::BitsToZero, E::None, B::None),
        }
    }
}

/// Bank mode of a part: how its flash is organised, by hardware or by an option bit.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Banks {
    /// One bank.
    Single,
    /// Two banks of half the flash each.
    Dual,
}

/// The part on the board: what varies inside a series.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Part {
    /// Sub-family.
    pub line: Line,
    /// Flash size in bytes, both banks together.
    pub flash: u32,
    /// Bank mode.
    pub banks: Banks,
    /// The erase size the application knows (from its HAL): must be what the series rules give.
    pub erase: Option<u32>,
    /// The write size of the flash driver, when it is not the default of the line: a multiple of the line's unit.
    pub write: Option<u32>,
}

/// One flash region as a HAL lists it, for [`Part::from_regions`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BankRun {
    /// 1 or 2; anything else (OTP) is skipped.
    pub bank: u8,
    /// Size in bytes.
    pub len: u32,
    /// Erase size.
    pub erase: u32,
    /// Write size of the driver.
    pub write: u32,
}

impl BankRun {
    /// A run that is skipped.
    pub const NONE: BankRun = BankRun {
        bank: 0,
        len: 0,
        erase: 0,
        write: 0,
    };
}

impl Part {
    /// A part with one bank of `flash` bytes.
    pub const fn single(line: Line, flash: u32) -> Self {
        Self {
            line,
            flash,
            banks: Banks::Single,
            erase: None,
            write: None,
        }
    }

    /// A part with two banks, `flash` bytes together.
    pub const fn dual(line: Line, flash: u32) -> Self {
        Self {
            line,
            flash,
            banks: Banks::Dual,
            erase: None,
            write: None,
        }
    }

    /// States the erase size, to have it checked against the series rules.
    pub const fn with_erase(mut self, erase: u32) -> Self {
        self.erase = Some(erase);
        self
    }

    /// Sets the write size of the flash driver.
    pub const fn with_write(mut self, write: u32) -> Self {
        self.write = Some(write);
        self
    }

    /// The part as a HAL's flash regions describe it: flash size, bank mode, erase and write size come from
    /// there and are then checked against the series rules. Uneven sectors (F4-style) are refused.
    ///
    /// Two banks on a line that has no bank mode to choose (F1 XL density) count as one flash.
    pub const fn from_regions(line: Line, regions: &[BankRun]) -> Result<Self, LayoutError> {
        let mut bank = [0u32; 2];
        let mut erase = 0;
        let mut write = 0;
        let mut i = 0;
        while i < regions.len() {
            let region = regions[i];
            i += 1;
            if region.bank != 1 && region.bank != 2 {
                continue;
            }
            if erase == 0 {
                erase = region.erase;
                write = region.write;
            }
            if region.erase != erase {
                return Err(LayoutError::UnevenSectors);
            }
            if region.write != write {
                return Err(LayoutError::WriteSize);
            }
            bank[region.bank as usize - 1] += region.len;
        }
        if bank[0] == 0 {
            return Err(LayoutError::FlashSize);
        }
        let one_flash = bank[1] == 0 || matches!(line, Line::F1 | Line::F1Connectivity);
        if !one_flash && bank[0] != bank[1] {
            return Err(LayoutError::FlashSize);
        }
        Ok(Self {
            line,
            flash: bank[0] + bank[1],
            banks: if one_flash { Banks::Single } else { Banks::Dual },
            erase: Some(erase),
            write: Some(write),
        })
    }

    /// Like [`from_regions`](Self::from_regions), stopping the build (in a `const`) with the reason.
    pub const fn checked_from_regions(line: Line, regions: &[BankRun]) -> Self {
        match Self::from_regions(line, regions) {
            Ok(part) => part,
            Err(e) => fail(e),
        }
    }
}

/// The part from embassy-stm32's generated flash metadata, so that flash size, erase size, write size and bank
/// layout are not typed a second time. `chunky` does not depend on the HAL: the macro reads
/// `embassy_stm32::flash::FLASH_REGIONS` in the crate that calls it.
///
/// ```ignore
/// const PART: Part = chunky::embassy_stm32_part!(Line::G0B1);
/// // or, when the HAL is renamed or re-exported:
/// const PART: Part = chunky::embassy_stm32_part!(Line::G0B1, my_bsp::hal::flash);
/// ```
///
/// On parts with a bank mode to choose, embassy-stm32's `single-bank` / `dual-bank` feature decides what the
/// regions say. Only the line is still typed, and a line that does not fit the regions stops the build.
#[macro_export]
macro_rules! embassy_stm32_part {
    ($line:expr) => {
        $crate::embassy_stm32_part!($line, embassy_stm32::flash)
    };
    ($line:expr, $($flash:ident)::+) => {{
        const REGIONS: usize = $($flash)::+::FLASH_REGIONS.len();
        const RUNS: [$crate::series::BankRun; REGIONS] = {
            let mut runs = [$crate::series::BankRun::NONE; REGIONS];
            let mut i = 0;
            while i < REGIONS {
                let region = $($flash)::+::FLASH_REGIONS[i];
                runs[i] = $crate::series::BankRun {
                    bank: match region.bank {
                        $($flash)::+::FlashBank::Bank1 => 1,
                        $($flash)::+::FlashBank::Bank2 => 2,
                        _ => 0,
                    },
                    len: region.size,
                    erase: region.erase_size,
                    write: region.write_size,
                };
                i += 1;
            }
            runs
        };
        $crate::series::Part::checked_from_regions($line, &RUNS)
    }};
}

/// How the device switches between images, and with it how the flash is divided.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Profile {
    /// A/B swap by embassy-boot (`chunky_swap`).
    Swap,
    /// Hardware bank swap (`chunky_bank`): needs a dual-bank part of a line that can pick its bank.
    BankSwap,
    /// RP235x boot ROM A/B partitions with try-before-you-buy (BOOT-3).
    RpBootrom,
    /// One image slot, updated in place by a small golden recovery image at its own address (BOOT-6).
    InPlaceGolden {
        /// Size of the golden image's region in bytes.
        golden: u32,
    },
}

/// How big the image slot is.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageSize {
    /// As big as the flash allows.
    Max,
    /// Exactly this many bytes: the same image, and the same geometry id, on parts with more flash.
    Exact(u32),
}

/// What the application chooses for its board.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Board {
    /// How images are switched.
    pub profile: Profile,
    /// Bytes for the bootloader at the start of the flash (of each bank, with bank swap). With the RP boot ROM:
    /// the partition table and whatever else comes before the slots.
    pub bootloader: u32,
    /// Bytes of an application config region at the end of the flash; 0 for none.
    pub config: u32,
    /// Delta and hash unit in bytes.
    pub chunk: u32,
    /// Size of the image slot.
    pub image: ImageSize,
    /// Keep a factory restore image in a slot of its own (BOOT-7).
    pub factory: bool,
}

/// Why a part and board cannot be laid out.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayoutError {
    /// The line is not of the series this build selected.
    OtherSeries,
    /// No part of the line has this flash size (zero, too big, not whole erase units or banks).
    FlashSize,
    /// The part does not exist in this bank mode.
    BankMode,
    /// The stated erase size is not what the series rules give for this part.
    EraseSizeMismatch,
    /// The flash has sectors of several sizes.
    UnevenSectors,
    /// The write size is not a multiple of the line's write unit, or regions disagree on it.
    WriteSize,
    /// Bank swap on a line that cannot pick the bank it boots from.
    BankSwapUnavailable,
    /// Bank swap on a part in single-bank mode.
    BankSwapOnSingleBank,
    /// The RP boot ROM profile on another series.
    RpBootromOffRp,
    /// The two banks are not one after the other, and the profile needs one stretch of flash.
    SplitFlash,
    /// The bootloader (or the golden image) has no room given.
    EmptyRegion,
    /// A given size is not whole erase units.
    NotEraseUnits,
    /// The image slot does not fit, or there is no room for one.
    ImageDoesNotFit,
    /// The image fits, but not the factory slot as well.
    FactoryDoesNotFit,
    /// The computed layout breaks a geometry rule.
    Geometry(GeometryError),
}

/// The two banks of a dual-bank part.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BankSpan {
    /// Size of one bank in bytes.
    pub len: u32,
    /// Distance from the first bank to the second: more than `len` where there is a hole between them
    /// (1 MB H7 parts, G47x below 512 KB).
    pub pitch: u32,
}

/// The partition table of one device, computed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Layout {
    /// The part it is for.
    pub part: Part,
    /// The profile it is for.
    pub profile: Profile,
    /// What is fixed for the part's line.
    pub facts: Facts,
    /// Delta and hash unit in bytes.
    pub chunk: u32,
    /// Erase unit in bytes.
    pub erase: u32,
    /// Write unit in bytes.
    pub write: u32,
    /// The banks of a dual-bank part.
    pub bank: Option<BankSpan>,
    /// The bootloader. With bank swap the same span of the second bank holds the same bytes.
    pub bootloader: Region,
    /// The golden recovery image (in-place profile).
    pub golden: Option<Region>,
    /// Boot state of the backend.
    pub state: Region,
    /// Version floor counter.
    pub floor: Region,
    /// The running image.
    pub active: Region,
    /// Where an update is staged; `None` in place.
    pub dfu: Option<Region>,
    /// The factory restore image. With bank swap the same span exists in the second bank.
    pub factory: Option<Region>,
    /// The application's config region.
    pub config: Option<Region>,
    /// Bytes of flash nothing is assigned to (of the first bank, with bank swap).
    pub spare: u32,
}

/// What the line's rules say about one part.
struct Rules {
    erase: u32,
    bank: Option<BankSpan>,
}

const fn rules(part: &Part) -> Result<Rules, LayoutError> {
    let dual = matches!(part.banks, Banks::Dual);
    let flash = part.flash;
    // (erase size, biggest flash, single bank exists, dual bank exists, distance of the banks or 0 for adjacent)
    let (erase, max, single_ok, dual_ok, pitch) = match part.line {
        Line::F1 => (if flash <= 128 * KB { KB } else { 2 * KB }, MB, true, false, 0),
        Line::F1Connectivity => (2 * KB, 256 * KB, true, false, 0),
        Line::G0 => (2 * KB, 128 * KB, true, false, 0),
        Line::G0B1 => (2 * KB, 512 * KB, true, flash >= 256 * KB, 0),
        Line::G4Cat2 => (2 * KB, 128 * KB, true, false, 0),
        Line::G4Cat3 => (if dual { 2 * KB } else { 4 * KB }, 512 * KB, true, true, 256 * KB),
        Line::G4Cat4 => (2 * KB, 512 * KB, true, false, 0),
        Line::L4 => (2 * KB, 512 * KB, true, false, 0),
        Line::L4Dual => (2 * KB, MB, true, true, 0),
        Line::L4Plus => (if dual { 4 * KB } else { 8 * KB }, 2 * MB, true, true, 0),
        Line::H72x => (128 * KB, MB, true, false, 0),
        Line::H74x | Line::H7Ax => {
            let erase = if matches!(part.line, Line::H74x) {
                128 * KB
            } else {
                8 * KB
            };
            if flash != 128 * KB && flash != MB && flash != 2 * MB {
                return Err(LayoutError::FlashSize);
            }
            (erase, 2 * MB, flash == 128 * KB, flash != 128 * KB, MB)
        }
        Line::Rp235x => (4 * KB, 16 * MB, true, false, 0),
    };
    if flash == 0 || flash > max || !flash.is_multiple_of(erase) {
        return Err(LayoutError::FlashSize);
    }
    if (dual && !dual_ok) || (!dual && !single_ok) {
        return Err(LayoutError::BankMode);
    }
    if let Some(stated) = part.erase
        && stated != erase
    {
        return Err(LayoutError::EraseSizeMismatch);
    }
    let bank = if dual {
        let len = flash / 2;
        if !len.is_multiple_of(erase) || (pitch != 0 && len > pitch) {
            return Err(LayoutError::FlashSize);
        }
        Some(BankSpan {
            len,
            pitch: if pitch == 0 { len } else { pitch },
        })
    } else {
        None
    };
    Ok(Rules { erase, bank })
}

/// Erase units of STATE for an image of `blocks` units.
const fn state_blocks(profile: Profile, blocks: u32, erase: u32, write: u32) -> u64 {
    match profile {
        // embassy-boot: a magic word, a validity word and 4 progress words per page.
        Profile::Swap => ((2 + 4 * blocks as u64) * write as u64).div_ceil(erase as u64),
        _ => 1,
    }
}

/// Erase units of everything that grows with the image, STATE included, for an image of `blocks` units.
const fn image_blocks(profile: Profile, factory: bool, blocks: u32, erase: u32, write: u32) -> u64 {
    let n = blocks as u64;
    let slots = match profile {
        // DFU is one erase unit longer than ACTIVE: the swap needs the room.
        Profile::Swap => 2 * n + 1,
        Profile::RpBootrom => 2 * n,
        // Per bank: the other slot is the other bank.
        Profile::BankSwap | Profile::InPlaceGolden { .. } => n,
    };
    let state = match profile {
        // STATE and FLOOR share the last unit of the two banks: counted with the fixed part.
        Profile::BankSwap => 0,
        _ => state_blocks(profile, blocks, erase, write),
    };
    state + slots + if factory { n } else { 0 }
}

/// The flash a profile divides, in erase units.
struct Room {
    profile: Profile,
    erase: u32,
    write: u32,
    total: u64,
    /// Units that do not grow with the image: bootloader, golden, config, and FLOOR (bank swap: the unit STATE
    /// and FLOOR share across the two banks).
    fixed: u64,
}

impl Room {
    const fn fits(&self, blocks: u32, factory: bool) -> bool {
        blocks > 0 && self.fixed + image_blocks(self.profile, factory, blocks, self.erase, self.write) <= self.total
    }
}

const fn region(offset: u32, len: u32) -> Region {
    Region { offset, len }
}

const fn optional(offset: u32, len: u32) -> Option<Region> {
    if len == 0 { None } else { Some(region(offset, len)) }
}

/// Computes the layout of `part` for `board`, for the series this build selected ([`SELECTED`]).
pub const fn layout(part: &Part, board: &Board) -> Result<Layout, LayoutError> {
    layout_for(SELECTED, part, board)
}

/// Computes the layout for a build of series `selected`; `None` lays out a part of any series, as host tools and
/// the worked examples in [`crate::chips`] need.
pub const fn layout_for(selected: Option<Series>, part: &Part, board: &Board) -> Result<Layout, LayoutError> {
    let facts = part.line.facts();
    if let Some(series) = selected
        && series as u8 != facts.series as u8
    {
        return Err(LayoutError::OtherSeries);
    }
    let Rules { erase, bank } = match rules(part) {
        Ok(rules) => rules,
        Err(e) => return Err(e),
    };
    let write = match part.write {
        Some(write) => write,
        None => facts.write,
    };
    if write == 0 || !write.is_multiple_of(facts.unit) {
        return Err(LayoutError::WriteSize);
    }
    if let Err(e) = FlashGeometry::validate_sizes(facts.family, board.chunk, erase, write) {
        return Err(LayoutError::Geometry(e));
    }

    let profile = board.profile;
    // The stretch of flash the profile divides: one bank with bank swap, else all of it.
    let (area, pitch) = match profile {
        Profile::BankSwap => {
            if matches!(facts.bank_select, BankSelect::None) {
                return Err(LayoutError::BankSwapUnavailable);
            }
            match bank {
                Some(bank) => (bank.len, bank.pitch),
                None => return Err(LayoutError::BankSwapOnSingleBank),
            }
        }
        Profile::Swap | Profile::RpBootrom | Profile::InPlaceGolden { .. } => {
            if matches!(profile, Profile::RpBootrom) && !matches!(facts.series, Series::Rp235x) {
                return Err(LayoutError::RpBootromOffRp);
            }
            if let Some(bank) = bank
                && bank.pitch != bank.len
            {
                return Err(LayoutError::SplitFlash);
            }
            (part.flash, 0)
        }
    };
    let golden = match profile {
        Profile::InPlaceGolden { golden } => {
            if golden == 0 {
                return Err(LayoutError::EmptyRegion);
            }
            golden
        }
        _ => 0,
    };
    if board.bootloader == 0 {
        return Err(LayoutError::EmptyRegion);
    }
    if !board.bootloader.is_multiple_of(erase) || !board.config.is_multiple_of(erase) || !golden.is_multiple_of(erase) {
        return Err(LayoutError::NotEraseUnits);
    }

    // From here on in erase units.
    let total = (area / erase) as u64;
    let fixed = (board.bootloader / erase) as u64 + (golden / erase) as u64 + (board.config / erase) as u64 + 1;
    let room = Room {
        profile,
        erase,
        write,
        total,
        fixed,
    };
    let blocks = match board.image {
        ImageSize::Exact(len) => {
            if !len.is_multiple_of(erase) {
                return Err(LayoutError::NotEraseUnits);
            }
            len / erase
        }
        ImageSize::Max => {
            // STATE grows with the image, so walk down from the most the slots alone would allow.
            let mut blocks = total as u32;
            while blocks > 1 && !room.fits(blocks, board.factory) {
                blocks -= 1;
            }
            blocks
        }
    };
    if !room.fits(blocks, board.factory) {
        return Err(if board.factory && room.fits(blocks, false) {
            LayoutError::FactoryDoesNotFit
        } else {
            LayoutError::ImageDoesNotFit
        });
    }

    let image = blocks * erase;
    let spare = (total - fixed - image_blocks(profile, board.factory, blocks, erase, write)) as u32 * erase;
    let factory = if board.factory { image } else { 0 };
    let bootloader = region(0, board.bootloader);
    let config = optional(area - board.config, board.config);
    let (golden, state, floor, active, dfu, factory) = match profile {
        Profile::BankSwap => {
            let active = region(board.bootloader, image);
            let last = area - erase;
            (
                None,
                region(last, erase),
                region(pitch + last, erase),
                active,
                Some(region(pitch + active.offset, image)),
                optional(active.offset + image, factory),
            )
        }
        Profile::Swap | Profile::RpBootrom | Profile::InPlaceGolden { .. } => {
            let state = region(
                board.bootloader + golden,
                state_blocks(profile, blocks, erase, write) as u32 * erase,
            );
            let floor = region(state.offset + state.len, erase);
            let active = region(floor.offset + erase, image);
            let dfu = match profile {
                Profile::Swap => image + erase,
                Profile::RpBootrom => image,
                _ => 0,
            };
            (
                optional(board.bootloader, golden),
                state,
                floor,
                active,
                optional(active.offset + image, dfu),
                optional(active.offset + image + dfu, factory),
            )
        }
    };
    // With bank swap the config region sits before the unit of STATE.
    let config = match (profile, config) {
        (Profile::BankSwap, Some(config)) => Some(region(config.offset - erase, config.len)),
        _ => config,
    };
    let layout = Layout {
        part: *part,
        profile,
        facts,
        chunk: board.chunk,
        erase,
        write,
        bank,
        bootloader,
        golden,
        state,
        floor,
        active,
        dfu,
        factory,
        config,
        spare,
    };
    if layout.dfu.is_some() {
        let (first, second) = layout.runs();
        let checked = match second {
            Some(second) => layout.geometry().validate_on(&[first, second]),
            None => layout.geometry().validate_on(&[first]),
        };
        if let Err(e) = checked {
            return Err(LayoutError::Geometry(e));
        }
    } else if active.len / board.chunk > u16::MAX as u32 {
        return Err(LayoutError::Geometry(GeometryError::TooManyChunks));
    }
    Ok(layout)
}

impl Layout {
    /// Computes the layout, or stops the build (in a `const`) with the reason.
    pub const fn checked(part: Part, board: Board) -> Self {
        match layout(&part, &board) {
            Ok(layout) => layout,
            Err(e) => fail(e),
        }
    }

    /// Like [`checked`](Self::checked) for a part of any series, whatever this build selected: for host tools and
    /// the worked examples in [`crate::chips`]. A firmware uses [`checked`](Self::checked), which holds its part
    /// to its series.
    pub const fn checked_for_any_series(part: Part, board: Board) -> Self {
        match layout_for(None, &part, &board) {
            Ok(layout) => layout,
            Err(e) => fail(e),
        }
    }

    /// The flash the part has: one run, or one per bank where the banks are apart.
    pub const fn runs(&self) -> (EraseRun, Option<EraseRun>) {
        let erase = self.erase;
        match self.bank {
            Some(BankSpan { len, pitch }) if pitch != len => (
                EraseRun { start: 0, len, erase },
                Some(EraseRun {
                    start: pitch,
                    len,
                    erase,
                }),
            ),
            _ => (
                EraseRun {
                    start: 0,
                    len: self.part.flash,
                    erase,
                },
                None,
            ),
        }
    }

    /// The geometry the device, the server and the boot backend work with.
    ///
    /// The in-place profile has no DFU: until its backend exists (BOOT-6) this stops the build. The golden,
    /// factory and config regions are not part of the geometry or its id.
    pub const fn geometry(&self) -> FlashGeometry {
        let Some(dfu) = self.dfu else {
            panic!("chunky layout: the in-place profile has no DFU and no backend yet (BOOT-6): no FlashGeometry")
        };
        FlashGeometry {
            family: self.facts.family,
            backend: match self.profile {
                Profile::Swap | Profile::InPlaceGolden { .. } => BackendKind::Swap,
                Profile::BankSwap => BackendKind::Bank,
                Profile::RpBootrom => BackendKind::RpBootrom,
            },
            chunk: self.chunk,
            erase: self.erase,
            write: self.write,
            state: self.state,
            floor: self.floor,
            active: self.active,
            dfu,
        }
    }
}

const fn fail(error: LayoutError) -> ! {
    match error {
        LayoutError::OtherSeries => {
            panic!("chunky layout: this line is not of the series the build selected (series-* feature)")
        }
        LayoutError::FlashSize => panic!(
            "chunky layout: no part of this line has this flash size (zero, too big, or not whole erase units or banks)"
        ),
        LayoutError::BankMode => {
            panic!("chunky layout: this part does not exist in this bank mode (single-bank only, or dual-bank only)")
        }
        LayoutError::EraseSizeMismatch => {
            panic!("chunky layout: erase size mismatch: the series rules give another erase size for this part")
        }
        LayoutError::UnevenSectors => {
            panic!("chunky layout: the flash has sectors of several sizes (uneven sectors): no series rule covers it")
        }
        LayoutError::WriteSize => {
            panic!("chunky layout: the write size is not a multiple of the line's write unit, or regions disagree")
        }
        LayoutError::BankSwapUnavailable => {
            panic!("chunky layout: bank swap on a line without hardware bank swap")
        }
        LayoutError::BankSwapOnSingleBank => {
            panic!("chunky layout: bank swap on a single-bank part: it needs the part in dual-bank mode")
        }
        LayoutError::RpBootromOffRp => panic!("chunky layout: the RP boot ROM profile needs series RP235x"),
        LayoutError::SplitFlash => panic!(
            "chunky layout: the two banks are not adjacent: use bank swap, or lay out one bank as a single-bank part"
        ),
        LayoutError::EmptyRegion => panic!("chunky layout: the bootloader (or the golden image) has no room given"),
        LayoutError::NotEraseUnits => {
            panic!("chunky layout: bootloader, golden, config and image sizes must be whole erase units")
        }
        LayoutError::ImageDoesNotFit => panic!("chunky layout: the image slot does not fit in the flash"),
        LayoutError::FactoryDoesNotFit => {
            panic!("chunky layout: the factory image does not fit: the flash has no room for one more image slot")
        }
        LayoutError::Geometry(e) => geometry::fail(e),
    }
}

#[cfg(test)]
mod tests;
