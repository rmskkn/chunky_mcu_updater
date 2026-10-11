use super::*;
use crate::chips;
use crate::geometry::BANK_RECORD_UNITS;
use std::collections::BTreeMap;
use std::format;
use std::vec;
use std::vec::Vec;

const LINES: [Line; 14] = [
    Line::F1,
    Line::F1Connectivity,
    Line::G0,
    Line::G0B1,
    Line::G4Cat2,
    Line::G4Cat3,
    Line::G4Cat4,
    Line::L4,
    Line::L4Dual,
    Line::L4Plus,
    Line::H72x,
    Line::H74x,
    Line::H7Ax,
    Line::Rp235x,
];

fn board(profile: Profile, bootloader: u32, chunk: u32) -> Board {
    Board {
        profile,
        bootloader,
        config: 0,
        chunk,
        image: ImageSize::Max,
        factory: false,
    }
}

/// The presets that existed before the series rules keep their numbers: these are the hand-made tables.
#[test]
fn old_presets_keep_their_layout() {
    let f103 = FlashGeometry {
        family: Family::Stm32F1,
        backend: BackendKind::Swap,
        chunk: KB,
        erase: KB,
        write: 4,
        state: region(20 * KB, KB),
        floor: region(21 * KB, KB),
        active: region(22 * KB, 52 * KB),
        dfu: region(74 * KB, 53 * KB),
    };
    assert_eq!(chips::STM32F103CB, f103);
    let h743 = FlashGeometry {
        family: Family::Stm32H7,
        backend: BackendKind::Bank,
        chunk: 2 * KB,
        erase: 128 * KB,
        write: 32,
        state: region(7 * 128 * KB, 128 * KB),
        floor: region(15 * 128 * KB, 128 * KB),
        active: region(128 * KB, 6 * 128 * KB),
        dfu: region(9 * 128 * KB, 6 * 128 * KB),
    };
    assert_eq!(chips::STM32H743ZI, h743);
    assert_eq!(chips::STM32H743ZI.id(), h743.id());
}

#[test]
fn worked_examples() {
    let l = chips::STM32G071RB_LAYOUT;
    assert_eq!((l.erase, l.write, l.bank), (2 * KB, 8, None));
    assert_eq!(l.active, region(20 * KB, 52 * KB));
    assert_eq!(l.dfu, Some(region(72 * KB, 54 * KB)));
    assert_eq!(l.spare, 2 * KB);

    // One bank or two, the swap layout of the G0B1RE is the same flash.
    let single = chips::STM32G0B1RE_LAYOUT;
    assert_eq!(single.state, region(24 * KB, 4 * KB));
    assert_eq!(single.active, region(30 * KB, 240 * KB));
    assert_eq!(single.dfu, Some(region(270 * KB, 242 * KB)));
    let dual = Layout::checked(Chip::dual(Line::G0B1, 512 * KB), board(Profile::Swap, 24 * KB, 2 * KB));
    assert_eq!(dual.geometry(), single.geometry());

    let bank = chips::STM32G0B1RE_BANK_LAYOUT;
    assert_eq!(
        bank.bank,
        Some(BankSpan {
            len: 256 * KB,
            pitch: 256 * KB
        })
    );
    assert_eq!(bank.active, region(24 * KB, 230 * KB));
    assert_eq!(bank.dfu, Some(region(280 * KB, 230 * KB)));
    assert_eq!(bank.state, region(254 * KB, 2 * KB));
    assert_eq!(bank.floor, region(510 * KB, 2 * KB));
    assert_eq!(bank.geometry().backend, BackendKind::Bank);

    let factory = chips::STM32G0B1RE_FACTORY_LAYOUT;
    assert_eq!(factory.active.len, 160 * KB);
    assert_eq!(factory.factory, Some(region(352 * KB, 160 * KB)));

    let h7a3 = chips::STM32H7A3ZI_LAYOUT;
    assert_eq!((h7a3.erase, h7a3.facts.unit, h7a3.write), (8 * KB, 16, 32));
    assert_eq!(h7a3.active, region(64 * KB, 952 * KB));
    assert_eq!(h7a3.dfu, Some(region(MB + 64 * KB, 952 * KB)));

    let rp = chips::RP2354_LAYOUT;
    assert_eq!(rp.active.len, 988 * KB);
    assert_eq!(rp.dfu.map(|dfu| dfu.len), Some(988 * KB));
    assert_eq!(rp.geometry().backend, BackendKind::RpBootrom);

    // In place with a golden image: no DFU, and most of the F103CB for the application.
    let golden = chips::STM32F103CB_GOLDEN_LAYOUT;
    assert_eq!(golden.golden, Some(region(20 * KB, 16 * KB)));
    assert_eq!(golden.active, region(38 * KB, 90 * KB));
    assert_eq!(golden.dfu, None);

    for (name, geometry) in chips::PRESETS {
        let (_, layout) = chips::LAYOUTS
            .iter()
            .find(|(n, _)| *n == name)
            .expect("a layout per preset");
        assert_eq!(layout.geometry(), *geometry);
    }
}

/// Real parts with the flash embassy-stm32's metadata (stm32-data) lists for them: flash size, erase size,
/// write size, and where the second bank starts.
#[test]
fn rules_agree_with_real_parts() {
    // (part, line, bank 1 bytes, bank 2 bytes, erase, driver write size, offset of bank 2)
    let parts: [(&str, Line, u32, u32, u32, u32, u32); 40] = [
        ("STM32F103C6", Line::F1, 32 * KB, 0, KB, 4, 0),
        ("STM32F103CB", Line::F1, 128 * KB, 0, KB, 4, 0),
        ("STM32F100ZC", Line::F1, 256 * KB, 0, 2 * KB, 4, 0),
        ("STM32F103ZE", Line::F1, 512 * KB, 0, 2 * KB, 4, 0),
        ("STM32F103ZG", Line::F1, 512 * KB, 512 * KB, 2 * KB, 4, 512 * KB),
        ("STM32F105R8", Line::F1Connectivity, 64 * KB, 0, 2 * KB, 4, 0),
        ("STM32F107VC", Line::F1Connectivity, 256 * KB, 0, 2 * KB, 4, 0),
        ("STM32G071RB", Line::G0, 128 * KB, 0, 2 * KB, 8, 0),
        ("STM32G0B1KB", Line::G0B1, 128 * KB, 0, 2 * KB, 8, 0),
        ("STM32G0B1CC single", Line::G0B1, 256 * KB, 0, 2 * KB, 8, 0),
        ("STM32G0B1CC dual", Line::G0B1, 128 * KB, 128 * KB, 2 * KB, 8, 128 * KB),
        ("STM32G0B1RE", Line::G0B1, 256 * KB, 256 * KB, 2 * KB, 8, 256 * KB),
        ("STM32G431KB", Line::G4Cat2, 128 * KB, 0, 2 * KB, 8, 0),
        ("STM32G474RE single", Line::G4Cat3, 512 * KB, 0, 4 * KB, 8, 0),
        (
            "STM32G474RE dual",
            Line::G4Cat3,
            256 * KB,
            256 * KB,
            2 * KB,
            8,
            256 * KB,
        ),
        ("STM32G474RB single", Line::G4Cat3, 128 * KB, 0, 4 * KB, 8, 0),
        ("STM32G474RB dual", Line::G4Cat3, 64 * KB, 64 * KB, 2 * KB, 8, 256 * KB),
        (
            "STM32G471CC dual",
            Line::G4Cat3,
            128 * KB,
            128 * KB,
            2 * KB,
            8,
            256 * KB,
        ),
        ("STM32G491RE", Line::G4Cat4, 512 * KB, 0, 2 * KB, 8, 0),
        ("STM32L432KC", Line::L4, 256 * KB, 0, 2 * KB, 8, 0),
        ("STM32L452RE", Line::L4, 512 * KB, 0, 2 * KB, 8, 0),
        ("STM32L476RC", Line::L4Dual, 128 * KB, 128 * KB, 2 * KB, 8, 128 * KB),
        ("STM32L471RE", Line::L4Dual, 256 * KB, 256 * KB, 2 * KB, 8, 256 * KB),
        ("STM32L476RG", Line::L4Dual, 512 * KB, 512 * KB, 2 * KB, 8, 512 * KB),
        ("STM32L496ZG", Line::L4Dual, 512 * KB, 512 * KB, 2 * KB, 8, 512 * KB),
        ("STM32L4P5ZE single", Line::L4Plus, 512 * KB, 0, 8 * KB, 8, 0),
        (
            "STM32L4P5ZE dual",
            Line::L4Plus,
            256 * KB,
            256 * KB,
            4 * KB,
            8,
            256 * KB,
        ),
        (
            "STM32L4R5ZG dual",
            Line::L4Plus,
            512 * KB,
            512 * KB,
            4 * KB,
            8,
            512 * KB,
        ),
        ("STM32L4R5ZI single", Line::L4Plus, 2 * MB, 0, 8 * KB, 8, 0),
        ("STM32L4R5ZI dual", Line::L4Plus, MB, MB, 4 * KB, 8, MB),
        ("STM32H730VB", Line::H72x, 128 * KB, 0, 128 * KB, 32, 0),
        ("STM32H725RE", Line::H72x, 512 * KB, 0, 128 * KB, 32, 0),
        ("STM32H723ZG", Line::H72x, MB, 0, 128 * KB, 32, 0),
        ("STM32H750VB", Line::H74x, 128 * KB, 0, 128 * KB, 32, 0),
        ("STM32H743ZG", Line::H74x, 512 * KB, 512 * KB, 128 * KB, 32, MB),
        ("STM32H743ZI", Line::H74x, MB, MB, 128 * KB, 32, MB),
        ("STM32H7B0VB", Line::H7Ax, 128 * KB, 0, 8 * KB, 32, 0),
        ("STM32H7A3ZG", Line::H7Ax, 512 * KB, 512 * KB, 8 * KB, 32, MB),
        ("STM32H7A3ZI", Line::H7Ax, MB, MB, 8 * KB, 32, MB),
        ("STM32H7B3LI", Line::H7Ax, MB, MB, 8 * KB, 32, MB),
    ];
    for (name, line, bank1, bank2, erase, write, second) in parts {
        let mut regions = vec![BankRun {
            bank: 1,
            len: bank1,
            erase,
            write,
        }];
        if bank2 != 0 {
            regions.push(BankRun {
                bank: 2,
                len: bank2,
                erase,
                write,
            });
        }
        // OTP, as embassy-stm32 lists it: skipped.
        regions.push(BankRun {
            bank: 0,
            len: 1024,
            erase: 4,
            write: 4,
        });
        let part = Chip::from_regions(line, &regions).unwrap_or_else(|e| panic!("{name}: {e:?}"));
        assert_eq!(part.flash, bank1 + bank2, "{name}");
        let rules = rules(&part).unwrap_or_else(|e| panic!("{name}: {e:?}"));
        assert_eq!(rules.erase, erase, "{name}");
        assert_eq!(part.line.facts().write, write, "{name}");
        match rules.bank {
            Some(bank) => assert_eq!((bank.len, bank.pitch), (bank2, second), "{name}"),
            // The XL-density F1 has two banks in a row and nothing to do with them.
            None => assert!(bank2 == 0 || line == Line::F1, "{name}"),
        }
        // And the wrong erase size for the part is caught.
        assert_eq!(
            layout_for(
                None,
                &part.with_erase(erase * 2),
                &board(Profile::Swap, 128 * KB, 2 * KB)
            ),
            Err(LayoutError::EraseSizeMismatch),
            "{name}"
        );
    }
}

fn each_region(layout: &Layout) -> Vec<(&'static str, Region)> {
    let mut regions = vec![
        ("bootloader", layout.bootloader),
        ("state", layout.state),
        ("floor", layout.floor),
        ("active", layout.active),
    ];
    for (name, region) in [
        ("golden", layout.golden),
        ("dfu", layout.dfu),
        ("factory", layout.factory),
        ("config", layout.config),
    ] {
        if let Some(region) = region {
            regions.push((name, region));
        }
    }
    regions
}

/// Checks a layout against what its part and board asked for, without the code that computed it.
fn check(layout: &Layout, part: &Chip, board: &Board, what: &str) {
    let erase = layout.erase;
    let (first, second) = layout.runs();
    let runs: Vec<EraseRun> = [Some(first), second].into_iter().flatten().collect();
    assert_eq!(runs.iter().map(|run| run.len).sum::<u32>(), part.flash, "{what}");
    let regions = each_region(layout);
    for (name, region) in &regions {
        assert!(region.len > 0, "{what}: {name} is empty");
        assert!(
            region.offset.is_multiple_of(erase) && region.len.is_multiple_of(erase),
            "{what}: {name} is not on erase units"
        );
        assert!(
            runs.iter()
                .any(|run| region.offset >= run.start && region.end() <= run.start as u64 + run.len as u64),
            "{what}: {name} is outside the flash"
        );
        for (other, other_region) in &regions {
            assert!(
                name == other
                    || region.end() <= other_region.offset as u64
                    || other_region.end() <= region.offset as u64,
                "{what}: {name} overlaps {other}"
            );
        }
    }
    assert_eq!(layout.bootloader, region(0, board.bootloader), "{what}");
    assert_eq!(layout.config.map_or(0, |config| config.len), board.config, "{what}");
    assert_eq!(layout.factory.is_some(), board.factory, "{what}");
    if let Some(factory) = layout.factory {
        assert_eq!(factory.len, layout.active.len, "{what}");
    }
    if let ImageSize::Exact(len) = board.image {
        assert_eq!(layout.active.len, len, "{what}");
    }
    assert!(layout.active.len / layout.chunk <= u16::MAX as u32, "{what}");
    let write_units = |region: Region| (region.len / layout.write) as u64;

    // Everything the profile divides, counted once.
    let area = match (board.profile, layout.bank) {
        (Profile::BankSwap, Some(bank)) => bank.len,
        _ => part.flash,
    };
    let in_area: u32 = regions
        .iter()
        .filter(|(_, region)| region.end() <= area as u64)
        .map(|(_, region)| region.len)
        .sum();
    assert!(layout.spare.is_multiple_of(erase), "{what}");

    match board.profile {
        Profile::Swap => {
            let dfu = layout.dfu.expect("swap has a DFU");
            assert_eq!(dfu.len, layout.active.len + erase, "{what}");
            assert!(
                write_units(layout.state) >= 2 + 4 * (layout.active.len / erase) as u64,
                "{what}: STATE cannot hold the swap progress"
            );
            assert_eq!(in_area + layout.spare, area, "{what}");
        }
        Profile::RpBootrom => {
            assert_eq!(layout.dfu.expect("two slots").len, layout.active.len, "{what}");
            assert_eq!(in_area + layout.spare, area, "{what}");
        }
        Profile::InPlaceGolden { golden } => {
            assert_eq!(layout.dfu, None, "{what}");
            assert_eq!(layout.golden.expect("a golden region").len, golden, "{what}");
            assert_eq!(in_area + layout.spare, area, "{what}");
        }
        Profile::BankSwap => {
            let bank = layout.bank.expect("bank swap has banks");
            let dfu = layout.dfu.expect("bank swap has a DFU");
            assert_eq!(part.banks, Banks::Dual, "{what}");
            assert_ne!(layout.facts.bank_select, BankSelect::None, "{what}");
            assert_eq!(
                dfu,
                region(layout.active.offset + bank.pitch, layout.active.len),
                "{what}"
            );
            // STATE closes the first bank and FLOOR the second.
            assert_eq!(layout.state, region(bank.len - erase, erase), "{what}");
            assert_eq!(layout.floor, region(bank.pitch + bank.len - erase, erase), "{what}");
            assert!(write_units(layout.state) >= 2 * BANK_RECORD_UNITS as u64, "{what}");
            // Bootloader, image, factory, config and STATE are in the first bank; nothing else is in the
            // second but the other slot and FLOOR.
            assert_eq!(in_area + layout.spare, area, "{what}");
            for (name, region) in &regions {
                if *name != "dfu" && *name != "floor" {
                    assert!(region.end() <= bank.len as u64, "{what}: {name} leaves the first bank");
                }
            }
        }
    }
    if board.profile != Profile::BankSwap {
        if let Some(config) = layout.config {
            assert_eq!(config.end(), part.flash as u64, "{what}: config is not at the end");
        }
        assert!(
            layout.bank.is_none_or(|bank| bank.pitch == bank.len),
            "{what}: one stretch of flash is needed"
        );
    }
    if layout.dfu.is_some() {
        let geometry = layout.geometry();
        assert_eq!(geometry.validate_on(&runs), Ok(()), "{what}");
        assert_eq!(
            (geometry.chunk, geometry.erase, geometry.write),
            (board.chunk, erase, layout.write)
        );
    }
}

/// Every line, at sizes that exist and sizes that do not, in both bank modes, with every profile, with and
/// without a factory slot and a config region: a layout that comes out holds every rule, `Max` really is the
/// most that fits, and every refusal has its reason.
#[test]
fn layout_matrix() {
    let sizes = [
        0,
        16 * KB,
        64 * KB,
        100 * KB,
        128 * KB,
        256 * KB,
        512 * KB,
        MB,
        2 * MB,
        4 * MB,
        16 * MB,
        32 * MB,
    ];
    let mut laid_out = 0;
    let mut refused = BTreeMap::new();
    for line in LINES {
        let facts = line.facts();
        assert!(facts.write.is_multiple_of(facts.unit), "{line:?}");
        assert_eq!(
            facts.ecc == EccFault::None,
            matches!(facts.rule, WriteRule::OnceOrZero | WriteRule::BitsToZero)
        );
        for flash in sizes {
            for part in [Chip::single(line, flash), Chip::dual(line, flash)] {
                let Ok(rules) = rules(&part) else {
                    let error = layout_for(None, &part, &board(Profile::Swap, 128 * KB, 2 * KB));
                    assert!(
                        matches!(error, Err(LayoutError::FlashSize | LayoutError::BankMode)),
                        "{part:?}: {error:?}"
                    );
                    *refused.entry(format!("{:?}", error.unwrap_err())).or_insert(0) += 1;
                    continue;
                };
                let erase = rules.erase;
                let chunk = erase.min(2 * KB);
                let profiles = [
                    Profile::Swap,
                    Profile::BankSwap,
                    Profile::RpBootrom,
                    Profile::InPlaceGolden { golden: 2 * erase },
                ];
                for profile in profiles {
                    for factory in [false, true] {
                        for config in [0, erase, 3 * erase] {
                            for bootloader in [erase, 16 * KB.max(erase)] {
                                let board = Board {
                                    profile,
                                    bootloader,
                                    config,
                                    chunk,
                                    image: ImageSize::Max,
                                    factory,
                                };
                                let what = format!("{part:?} {board:?}");
                                match layout_for(None, &part, &board) {
                                    Ok(layout) => {
                                        laid_out += 1;
                                        assert_eq!((layout.erase, layout.bank), (erase, rules.bank), "{what}");
                                        check(&layout, &part, &board, &what);
                                        // The same size asked for by number is the same layout, one erase
                                        // unit more does not fit, and half of it leaves the rest spare.
                                        let exact = |len: u32| {
                                            let board = Board {
                                                image: ImageSize::Exact(len),
                                                ..board
                                            };
                                            (layout_for(None, &part, &board), board)
                                        };
                                        assert_eq!(exact(layout.active.len).0, Ok(layout), "{what}");
                                        assert!(
                                            matches!(
                                                exact(layout.active.len + erase).0,
                                                Err(LayoutError::ImageDoesNotFit | LayoutError::FactoryDoesNotFit)
                                            ),
                                            "{what}: Max is not the most that fits"
                                        );
                                        let half = (layout.active.len / erase).div_ceil(2) * erase;
                                        let (smaller, board) = exact(half);
                                        let smaller = smaller.unwrap_or_else(|e| panic!("{what}: half: {e:?}"));
                                        check(&smaller, &part, &board, &what);
                                        assert!(smaller.spare >= layout.spare, "{what}");
                                    }
                                    Err(error) => {
                                        let area = match (profile, rules.bank) {
                                            (Profile::BankSwap, Some(bank)) => bank.len,
                                            _ => flash,
                                        };
                                        // Bootloader, config, FLOOR (or the unit it shares with STATE), and
                                        // one erase unit for everything else the profile lays out.
                                        let needs = |factory: bool| {
                                            let slots = match profile {
                                                Profile::Swap => 4,
                                                Profile::RpBootrom => 3,
                                                Profile::BankSwap => 1,
                                                Profile::InPlaceGolden { .. } => 4,
                                            };
                                            bootloader + config + erase + (slots + factory as u32) * erase
                                        };
                                        let fits = match error {
                                            LayoutError::BankSwapUnavailable => {
                                                profile == Profile::BankSwap && facts.bank_select == BankSelect::None
                                            }
                                            LayoutError::BankSwapOnSingleBank => {
                                                profile == Profile::BankSwap && part.banks == Banks::Single
                                            }
                                            LayoutError::RpBootromOffRp => {
                                                profile == Profile::RpBootrom && facts.series != Series::Rp235x
                                            }
                                            LayoutError::SplitFlash => {
                                                profile != Profile::BankSwap
                                                    && rules.bank.is_some_and(|bank| bank.pitch != bank.len)
                                            }
                                            // Too little flash for the fixed regions and one block per slot.
                                            LayoutError::ImageDoesNotFit => needs(false) > area,
                                            LayoutError::FactoryDoesNotFit => {
                                                factory && needs(true) > area && needs(false) <= area
                                            }
                                            _ => false,
                                        };
                                        assert!(fits, "{what}: {error:?}");
                                        *refused.entry(format!("{error:?}")).or_insert(0) += 1;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    assert!(laid_out > 1000, "{laid_out} layouts");
    for reason in [
        "FlashSize",
        "BankMode",
        "BankSwapUnavailable",
        "BankSwapOnSingleBank",
        "RpBootromOffRp",
        "SplitFlash",
        "ImageDoesNotFit",
        "FactoryDoesNotFit",
    ] {
        assert!(
            refused.contains_key(reason),
            "the matrix never met {reason}: {refused:?}"
        );
    }
}

/// One case per reason a layout is refused: the build errors.
#[test]
fn every_inconsistency_has_a_named_refusal() {
    let g0 = board(Profile::Swap, 16 * KB, 2 * KB);
    let cases: [(&str, Chip, Board, LayoutError); 22] = [
        (
            "bank swap on a line with one bank",
            Chip::single(Line::G0, 128 * KB),
            board(Profile::BankSwap, 16 * KB, 2 * KB),
            LayoutError::BankSwapUnavailable,
        ),
        (
            "bank swap on a G0B1 left in single-bank mode",
            Chip::single(Line::G0B1, 512 * KB),
            board(Profile::BankSwap, 16 * KB, 2 * KB),
            LayoutError::BankSwapOnSingleBank,
        ),
        (
            "dual bank on a single-bank G0",
            Chip::dual(Line::G0, 128 * KB),
            g0,
            LayoutError::BankMode,
        ),
        (
            "dual bank on a 128 KB G0B1",
            Chip::dual(Line::G0B1, 128 * KB),
            g0,
            LayoutError::BankMode,
        ),
        (
            "an H743 is never single-bank",
            Chip::single(Line::H74x, 2 * MB),
            board(Profile::Swap, 128 * KB, 2 * KB),
            LayoutError::BankMode,
        ),
        (
            "an H750 is never dual-bank",
            Chip::dual(Line::H74x, 128 * KB),
            board(Profile::Swap, 128 * KB, 2 * KB),
            LayoutError::BankMode,
        ),
        (
            "no H7A3 has 256 KB",
            Chip::dual(Line::H7Ax, 256 * KB),
            board(Profile::BankSwap, 64 * KB, 2 * KB),
            LayoutError::FlashSize,
        ),
        (
            "no G0 has 1 MB",
            Chip::single(Line::G0B1, MB),
            g0,
            LayoutError::FlashSize,
        ),
        (
            "flash that is not whole pages",
            Chip::single(Line::G0, 127 * KB),
            g0,
            LayoutError::FlashSize,
        ),
        (
            "1 KB pages stated for a high-density F1",
            Chip::single(Line::F1, 512 * KB).with_erase(KB),
            board(Profile::Swap, 16 * KB, KB),
            LayoutError::EraseSizeMismatch,
        ),
        (
            "128 KB sectors stated for an H7A3",
            Chip::dual(Line::H7Ax, 2 * MB).with_erase(128 * KB),
            board(Profile::BankSwap, 128 * KB, 2 * KB),
            LayoutError::EraseSizeMismatch,
        ),
        (
            "2 KB pages stated for a G474 in single-bank mode",
            Chip::single(Line::G4Cat3, 512 * KB).with_erase(2 * KB),
            g0,
            LayoutError::EraseSizeMismatch,
        ),
        (
            "a write size of a half-word and a half",
            Chip::single(Line::F1, 128 * KB).with_write(3),
            board(Profile::Swap, 16 * KB, KB),
            LayoutError::WriteSize,
        ),
        (
            "the RP boot ROM on an STM32",
            Chip::single(Line::G0, 128 * KB),
            board(Profile::RpBootrom, 16 * KB, 2 * KB),
            LayoutError::RpBootromOffRp,
        ),
        (
            "embassy-boot swap across the hole of a 1 MB H743",
            Chip::dual(Line::H74x, MB),
            board(Profile::Swap, 128 * KB, 2 * KB),
            LayoutError::SplitFlash,
        ),
        (
            "no bootloader",
            Chip::single(Line::G0, 128 * KB),
            board(Profile::Swap, 0, 2 * KB),
            LayoutError::EmptyRegion,
        ),
        (
            "a bootloader of 5 KB in 2 KB pages",
            Chip::single(Line::G0, 128 * KB),
            board(Profile::Swap, 5 * KB, 2 * KB),
            LayoutError::NotEraseUnits,
        ),
        (
            "a 60 KB image on an F103CB with A/B swap",
            Chip::single(Line::F1, 128 * KB),
            Board {
                image: ImageSize::Exact(60 * KB),
                ..board(Profile::Swap, 20 * KB, KB)
            },
            LayoutError::ImageDoesNotFit,
        ),
        (
            "a factory image next to the 52 KB A/B slots of an F103CB",
            Chip::single(Line::F1, 128 * KB),
            Board {
                image: ImageSize::Exact(52 * KB),
                factory: true,
                ..board(Profile::Swap, 20 * KB, KB)
            },
            LayoutError::FactoryDoesNotFit,
        ),
        (
            "a factory image per bank of an H743ZI with six image sectors",
            Chip::dual(Line::H74x, 2 * MB),
            Board {
                image: ImageSize::Exact(6 * 128 * KB),
                factory: true,
                ..board(Profile::BankSwap, 128 * KB, 2 * KB)
            },
            LayoutError::FactoryDoesNotFit,
        ),
        (
            "a chunk that does not divide the page",
            Chip::single(Line::G0, 128 * KB),
            board(Profile::Swap, 16 * KB, 768),
            LayoutError::Geometry(GeometryError::ChunkNotDividingErase),
        ),
        (
            "1 KB chunks on 16 MB: more than a 16-bit index",
            Chip::single(Line::Rp235x, 16 * MB),
            board(Profile::InPlaceGolden { golden: 64 * KB }, 64 * KB, 64),
            LayoutError::Geometry(GeometryError::ChunkTooSmall),
        ),
    ];
    for (what, part, board, expected) in cases {
        assert_eq!(layout_for(None, &part, &board), Err(expected), "{what}");
    }
    // In place there is no geometry to count the chunks, so the layout does.
    assert_eq!(
        layout_for(
            None,
            &Chip::single(Line::Rp235x, 16 * MB),
            &board(Profile::InPlaceGolden { golden: 64 * KB }, 64 * KB, 128)
        ),
        Err(LayoutError::Geometry(GeometryError::TooManyChunks))
    );
}

/// The biggest flash there is, laid out in a `const`: the search for the image size stays cheap to evaluate.
#[test]
fn sixteen_megabytes_in_a_const() {
    const LAYOUT: Layout = Layout::checked_for_any_series(
        Chip::single(Line::Rp235x, 16 * MB),
        Board {
            profile: Profile::Swap,
            bootloader: 64 * KB,
            config: 64 * KB,
            chunk: 4 * KB,
            image: ImageSize::Max,
            factory: true,
        },
    );
    assert_eq!(LAYOUT.active.len, 5412 * KB);
    assert_eq!(LAYOUT.state.len, 8 * KB);
    assert_eq!(LAYOUT.spare, 4 * KB);
}

#[test]
fn a_selected_series_refuses_the_others() {
    let part = Chip::single(Line::G0, 128 * KB);
    let board = board(Profile::Swap, 16 * KB, 2 * KB);
    assert!(layout_for(Some(Series::Stm32G0), &part, &board).is_ok());
    assert_eq!(
        layout_for(Some(Series::Stm32G4), &part, &board),
        Err(LayoutError::OtherSeries)
    );
    // The tests run as a host build: no series is selected and every line can be laid out.
    assert_eq!(SELECTED, None);
    for line in LINES {
        assert_eq!(line.series(), line.facts().series);
    }
}

#[test]
fn hal_regions_with_uneven_sectors_are_refused() {
    let run = |bank, len, erase| BankRun {
        bank,
        len,
        erase,
        write: 4,
    };
    // An F4: four 16 KB sectors, one of 64 KB, then 128 KB ones.
    let f4 = [
        run(1, 64 * KB, 16 * KB),
        run(1, 64 * KB, 64 * KB),
        run(1, 896 * KB, 128 * KB),
    ];
    assert_eq!(Chip::from_regions(Line::F1, &f4), Err(LayoutError::UnevenSectors));
    // Banks of different sizes, and nothing but OTP.
    let lopsided = [run(1, 256 * KB, 2 * KB), run(2, 128 * KB, 2 * KB)];
    assert_eq!(Chip::from_regions(Line::G0B1, &lopsided), Err(LayoutError::FlashSize));
    assert_eq!(
        Chip::from_regions(Line::G0B1, &[run(0, 1024, 4)]),
        Err(LayoutError::FlashSize)
    );
    let mut mixed = [run(1, 256 * KB, 2 * KB), run(2, 256 * KB, 2 * KB)];
    mixed[1].write = 8;
    assert_eq!(Chip::from_regions(Line::G0B1, &mixed), Err(LayoutError::WriteSize));
}

/// The shape of `embassy_stm32::flash` (0.6.0) for an STM32G0B1RE: what [`embassy_stm32_chip!`] reads.
mod embassy_stm32 {
    pub mod flash {
        #![allow(dead_code)]
        pub enum FlashBank {
            Bank1 = 0,
            Bank2 = 1,
            Otp,
        }
        pub struct FlashRegion {
            pub bank: FlashBank,
            pub offset: u32,
            pub size: u32,
            pub erase_size: u32,
            pub write_size: u32,
            pub erase_value: u8,
        }
        const fn region(bank: FlashBank, size: u32, erase_size: u32, write_size: u32) -> FlashRegion {
            FlashRegion {
                bank,
                offset: 0,
                size,
                erase_size,
                write_size,
                erase_value: 0xff,
            }
        }
        pub const BANK1_REGION: FlashRegion = region(FlashBank::Bank1, 262_144, 2048, 8);
        pub const BANK2_REGION: FlashRegion = region(FlashBank::Bank2, 262_144, 2048, 8);
        pub const OTP_REGION: FlashRegion = region(FlashBank::Otp, 1024, 8, 8);
        pub const FLASH_REGIONS: [&FlashRegion; 3] = [&BANK1_REGION, &BANK2_REGION, &OTP_REGION];
    }
}

#[test]
fn part_from_embassy_stm32_metadata() {
    const PART: Chip = crate::embassy_stm32_chip!(Line::G0B1, embassy_stm32::flash);
    const DEFAULT_PATH: Chip = crate::embassy_stm32_chip!(Line::G0B1);
    assert_eq!(PART, Chip::dual(Line::G0B1, 512 * KB).with_erase(2048).with_write(8));
    assert_eq!(DEFAULT_PATH, PART);
    // Nothing typed twice, and the same layout as the worked example.
    const LAYOUT: Layout = Layout::checked(
        PART,
        Board {
            profile: Profile::BankSwap,
            bootloader: 24 * KB,
            config: 0,
            chunk: 2 * KB,
            image: ImageSize::Max,
            factory: false,
        },
    );
    assert_eq!(LAYOUT.geometry(), chips::STM32G0B1RE_BANK);
}
