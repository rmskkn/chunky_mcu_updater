//! The worked examples of `chunky::series` on the simulator: layouts computed from series, part and profile
//! carry a whole update and a revert on the flash model of their family, on the backend of their profile.

mod common;

use chunky::chips;
use chunky::geometry::FlashGeometry;
use chunky::series::{Board, Chip, ImageSize, Layout, Line, Profile};
use chunky_sim::{Backend, Bank, F1, FlashModel, G0, H7, H7Mini, Swap};
use common::{App, World, run_update, signer};

fn updates_and_reverts<M: FlashModel, B: Backend<M>>(geometry: FlashGeometry) {
    assert_eq!(
        (M::FAMILY, M::ERASE_SIZE as u32, M::WRITE_SIZE as u32),
        (geometry.family, geometry.erase, geometry.write),
        "the flash model is the one of this layout"
    );
    let world = World::with_geometry(geometry, signer());
    for app in [App::Good, App::Bad] {
        let device = world.device_on::<M, B>();
        let seen = run_update(&device, &world, app, &[]);
        assert_eq!((seen.sessions, seen.trials), (1, 1));
    }
}

#[test]
fn stm32f103cb_swap() {
    updates_and_reverts::<F1, Swap>(chips::STM32F103CB);
}

#[test]
fn stm32g071rb_swap() {
    updates_and_reverts::<G0, Swap>(chips::STM32G071RB);
}

#[test]
fn stm32g0b1re_swap() {
    updates_and_reverts::<G0, Swap>(chips::STM32G0B1RE);
}

#[test]
fn stm32g0b1re_bank_swap() {
    updates_and_reverts::<G0, Bank>(chips::STM32G0B1RE_BANK);
}

#[test]
fn stm32h7a3zi_bank_swap() {
    updates_and_reverts::<H7Mini, Bank>(chips::STM32H7A3ZI);
}

/// A 1 MB H743: two banks of four sectors with a hole between them, the second bank 1 MB after the first.
#[test]
fn stm32h743zg_bank_swap_across_the_hole() {
    const LAYOUT: Layout = Layout::checked_for_any_series(
        Chip::dual(Line::H74x, 1024 * 1024),
        Board {
            profile: Profile::BankSwap,
            bootloader: 128 * 1024,
            config: 0,
            chunk: 2048,
            image: ImageSize::Max,
            factory: false,
        },
    );
    assert_eq!(LAYOUT.active.len, 2 * 128 * 1024);
    assert_eq!(LAYOUT.geometry().bank_len(), 1024 * 1024);
    updates_and_reverts::<H7, Bank>(LAYOUT.geometry());
}

/// The factory slot and the config region are outside what the backend touches: the same update runs.
#[test]
fn stm32g0b1re_swap_with_factory_and_config() {
    const LAYOUT: Layout = Layout::checked_for_any_series(
        Chip::single(Line::G0B1, 512 * 1024),
        Board {
            profile: Profile::Swap,
            bootloader: 24 * 1024,
            config: 4 * 1024,
            chunk: 2048,
            image: ImageSize::Exact(96 * 1024),
            factory: true,
        },
    );
    assert_eq!(LAYOUT.factory.map(|factory| factory.len), Some(96 * 1024));
    updates_and_reverts::<G0, Swap>(LAYOUT.geometry());
}
