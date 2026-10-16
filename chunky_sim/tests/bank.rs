//! The hardware bank swap backend: the power-cut sweeps of `sweep.rs` on two banks and a swap bit, and what a
//! compromised application, a forged state log and a broken bank can and cannot do to it.

mod common;

use chunky::chips::STM32H743ZI;
use chunky::descriptor::ImageError;
use chunky::{ApplyError, BootState};
use chunky_bank::{BootOutcome, Rejected, UpdaterError};
use chunky_server::{Release, Signer, build_image};
use chunky_sim::{Bank, Device, FlashModel, G0, H7, H7Mini};
use common::{App, PRODUCT, World, bytes, meta, run_update, signer, sweep_from};
use embedded_storage::nor_flash::NorFlash;
use proptest::prelude::*;

/// Ways each cut is torn. `CHUNKY_SWEEP_SEEDS=500 just test` digs deeper.
fn seeds() -> u64 {
    std::env::var("CHUNKY_SWEEP_SEEDS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(16)
}

/// The device as it is after an odd number of updates: running from the second bank, swap bit set.
fn start_swapped<M: FlashModel>(device: &Device<M, Bank>) {
    let geometry = device.geometry;
    let mut flash = device.flash.borrow_mut();
    let image = flash.dump(geometry.active.offset as usize, geometry.active.len as usize);
    let leftovers = flash.dump(geometry.dfu.offset as usize, geometry.dfu.len as usize);
    flash.program_swap(true).unwrap();
    flash.reset();
    flash.load(geometry.active.offset as usize, &image);
    flash.load(geometry.dfu.offset as usize, &leftovers);
}

/// The device as it is when every record of its state log is used: the next boot erases the log.
fn fill_log<M: FlashModel>(device: &Device<M, Bank>) {
    device.boot().unwrap();
    let state = device.bank_map().state;
    let record = 8 * M::WRITE_SIZE;
    let used = vec![0; state.len as usize - record];
    device.flash.borrow_mut().load(state.offset as usize + record, &used);
    assert_eq!(device.boot_state(), Ok(BootState::Confirmed));
    assert_eq!(device.mark_staged_unchecked(), Err(UpdaterError::NoRecord));
}

macro_rules! sweeps {
    ($($name:ident: $model:ty, chunk $chunk:expr, blocks $blocks:expr, seeds x $deeper:expr, $prepare:expr;)*) => {
        $(
            mod $name {
                use super::*;

                #[test]
                fn good_release_survives_a_cut_anywhere() {
                    sweep_from::<$model, Bank>($chunk, $blocks, App::Good, seeds() * $deeper, $prepare);
                }

                #[test]
                fn bad_release_reverts_despite_a_cut_anywhere() {
                    sweep_from::<$model, Bank>($chunk, $blocks, App::Bad, seeds() * $deeper, $prepare);
                }
            }
        )*
    };
}

// A used-up log is erased with every record in it set. Few tears of that erase leave the header standing, so
// these runs tear each operation more ways: without the zeros over the header they fail at the default depth.
sweeps! {
    h7_small_chunks_in_big_sectors: H7Mini, chunk 2048, blocks 3, seeds x 1, |_| {};
    h7_from_the_second_bank: H7Mini, chunk 2048, blocks 3, seeds x 1, start_swapped;
    h7_log_used_up: H7Mini, chunk 2048, blocks 3, seeds x 8, fill_log;
    g4: G0, chunk 2048, blocks 8, seeds x 1, |_| {};
    g4_log_used_up: G0, chunk 1024, blocks 6, seeds x 8, fill_log;
}

fn cuts() -> impl Strategy<Value = Vec<(u64, u64)>> {
    prop::collection::vec((0u64..60, any::<u64>()), 1..5)
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(256))]

    /// Several cuts in one update, at random distances.
    #[test]
    fn repeated_cuts(cuts in cuts(), good in any::<bool>(), h7 in any::<bool>(), swapped in any::<bool>()) {
        let app = if good { App::Good } else { App::Bad };
        if h7 {
            let world = World::on::<H7Mini, Bank>(2048, 2, signer());
            let device = world.device_on::<H7Mini, Bank>();
            if swapped {
                start_swapped(&device);
            }
            run_update(&device, &world, app, &cuts);
        } else {
            let world = World::on::<G0, Bank>(2048, 4, signer());
            let device = world.device_on::<G0, Bank>();
            if swapped {
                start_swapped(&device);
            }
            run_update(&device, &world, app, &cuts);
        }
    }
}

type Model = H7Mini;
const WORD: usize = 32;

fn world() -> World {
    World::on::<Model, Bank>(2048, 3, signer())
}

/// A device that has booted once: its log is started and its floor is that of the old release.
fn booted(world: &World) -> Device<Model, Bank> {
    let device = world.device_on::<Model, Bank>();
    assert_eq!(running(&device), (1, BootState::Confirmed, None));
    assert_eq!(device.floor(), 1);
    device
}

/// One reset: what runs, in which state, and what was refused.
fn running(device: &Device<Model, Bank>) -> (u32, BootState, Option<Rejected>) {
    match device.boot().unwrap() {
        BootOutcome::Run { image, state, rejected } => (image.version, state, rejected),
        other => panic!("{other:?}"),
    }
}

/// The application takes the plan for the new release and stages it.
fn stage_new(world: &World, device: &Device<Model, Bank>) {
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    assert_eq!(device.apply(&plan.encode()).unwrap().version, 2);
    assert_eq!(device.boot_state(), Ok(BootState::Staged));
}

/// Sets flag `unit` of the log's record `record` the way a compromised application would: directly.
fn forge(device: &Device<Model, Bank>, record: u32, unit: u32) {
    device
        .part(device.bank_map().state)
        .write((record * 8 + unit) * WORD as u32, &[0; WORD])
        .unwrap();
}

const TO_PLAIN: u32 = 1;
const TO_SWAPPED: u32 = 2;
const TRIED: u32 = 3;

#[test]
fn an_update_is_one_bit_and_copies_nothing() {
    let mut world = world();
    let device = world.device_on::<Model, Bank>();
    run_update(&device, &world, App::Good, &[]);
    assert!(device.flash.borrow().swapped());
    // The old release is still where it ran, untouched: that is what a revert would have gone back to.
    assert!(device.dump(world.geometry.dfu) == world.old);
    assert_eq!(
        device.flash.borrow().erase_count(world.geometry.active.offset as usize),
        1
    );

    // A third release goes into the first bank again, and the bit goes back.
    let third = build_image(&bytes(4, 20_000), &world.geometry, &meta(3, 2), &world.signer).unwrap();
    let release = Release::add(&third, &world.geometry, &mut world.store).unwrap();
    let plan = world.plan_for(&device.manifest().unwrap(), &release);
    assert_eq!(device.apply(&plan.encode()).unwrap().version, 3);
    assert_eq!(running(&device), (3, BootState::Trial, None));
    device.confirm().unwrap();
    assert_eq!(running(&device), (3, BootState::Confirmed, None));
    assert!(!device.flash.borrow().swapped());
    assert!(device.dump(world.geometry.active) == third);
    assert!(device.dump(world.geometry.dfu) == world.new);

    // Two updates, and the state log was erased once: when the device first booted.
    let state = device.bank_map().state;
    assert_eq!(device.flash.borrow().erase_count(state.offset as usize), 1);
    assert_eq!(device.floor(), 2);
}

#[test]
fn unsigned_build_updates_and_reverts() {
    for app in [App::Good, App::Bad] {
        let world = World::on::<Model, Bank>(2048, 3, Signer::Unsigned);
        run_update(&world.device_on::<Model, Bank>(), &world, app, &[]);
    }
}

#[test]
fn a_reverted_update_stays_reported_until_the_next_one() {
    let world = world();
    let device = world.device_on::<Model, Bank>();
    run_update(&device, &world, App::Bad, &[]);
    for _ in 0..3 {
        assert_eq!(running(&device), (1, BootState::Reverted, None));
        assert_eq!(device.boot_state(), Ok(BootState::Reverted));
    }
    assert!(!device.flash.borrow().swapped());
    // The release that failed is still in the other bank: offering it again costs no transfer.
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    assert_eq!(plan.stats.wire_bytes, plan.encode().len());
    assert_eq!(plan.steps.len(), 0);
    device.apply(&plan.encode()).unwrap();
    assert_eq!(running(&device), (2, BootState::Trial, None));
}

#[test]
fn nothing_is_staged_while_an_update_is_in_flight() {
    let world = world();
    let device = booted(&world);
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release).encode();
    device.apply(&plan).unwrap();
    assert_eq!(device.apply(&plan).unwrap_err(), ApplyError::NotConfirmed);
    assert_eq!(running(&device), (2, BootState::Trial, None));
    assert_eq!(device.apply(&plan).unwrap_err(), ApplyError::NotConfirmed);
    assert_eq!(device.mark_staged_unchecked(), Err(UpdaterError::NoRecord));
}

#[test]
fn a_switch_asked_for_without_a_verified_image_is_refused() {
    let world = world();
    let device = booted(&world);
    // A compromised application asks for a switch to whatever lies in the other bank.
    device.mark_staged_unchecked().unwrap();
    let before = device.flash.borrow().ops();
    assert!(matches!(
        running(&device),
        (1, BootState::Confirmed, Some(Rejected::Image(_)))
    ));
    assert!(!device.flash.borrow().swap_programmed());
    // All the bootloader wrote is the flag that closes the request.
    assert_eq!(device.flash.borrow().ops(), before + 1);
    assert_eq!(running(&device), (1, BootState::Confirmed, None));
}

#[test]
fn the_old_release_in_the_other_bank_is_no_way_back_below_the_floor() {
    let world = world();
    let device = world.device_on::<Model, Bank>();
    run_update(&device, &world, App::Good, &[]);
    assert_eq!(device.floor(), 2);
    // The other bank holds the old release, whole and validly signed. Asking for it is refused.
    assert!(device.dump(world.geometry.dfu) == world.old);
    device.mark_staged_unchecked().unwrap();
    assert_eq!(
        running(&device),
        (2, BootState::Confirmed, Some(Rejected::Image(ImageError::RolledBack)))
    );
    // And so is the option bit set behind the bootloader's back: the old release does not run.
    device.flash.borrow_mut().program_swap(false).unwrap();
    assert_eq!(
        device.boot().unwrap(),
        BootOutcome::Recovery {
            reason: ImageError::RolledBack
        }
    );
}

#[test]
fn the_bit_is_not_programmed_when_the_other_bank_has_another_bootloader() {
    let world = world();
    let device = booted(&world);
    let other_loader = device.bank_map().other_loader;
    device
        .flash
        .borrow_mut()
        .load(other_loader.offset as usize + 100, &[0xEE]);
    stage_new(&world, &device);
    assert_eq!(running(&device), (1, BootState::Confirmed, Some(Rejected::Loader)));
    assert!(!device.flash.borrow().swap_programmed());
}

#[test]
fn an_image_that_stops_verifying_before_its_trial_is_left_without_running() {
    let world = world();
    let device = booted(&world);
    stage_new(&world, &device);
    assert_eq!(device.boot_once(), Ok(BootOutcome::Reset));
    // The bit is programmed and the reset has not happened yet. One word of the new image rots.
    device
        .flash
        .borrow_mut()
        .inject_ecc(world.geometry.dfu.offset as usize + 4096);
    assert_eq!(running(&device), (1, BootState::Reverted, None));
    assert!(!device.flash.borrow().swapped());
    assert_eq!(device.floor(), 1);
}

#[test]
fn a_trial_with_nothing_to_go_back_to_stays_a_trial() {
    let world = world();
    let device = booted(&world);
    stage_new(&world, &device);
    assert_eq!(running(&device), (2, BootState::Trial, None));
    // The old release rots while the new one is on trial, and the new one does not confirm.
    device.flash.borrow_mut().inject_ecc(world.geometry.dfu.offset as usize);
    for _ in 0..2 {
        assert_eq!(
            running(&device),
            (2, BootState::Trial, Some(Rejected::Image(ImageError::Unreadable)))
        );
        assert_eq!(device.floor(), 1, "an image on trial raised the floor");
    }
    device.confirm().unwrap();
    assert_eq!(running(&device), (2, BootState::Confirmed, None));
    assert_eq!(device.floor(), 2);
}

#[test]
fn forged_state_runs_nothing_unverified() {
    // Every way of setting the bootloader's flags behind its back, with junk in the other bank: the old
    // release keeps running, the bit stays, the floor stays.
    let forgeries: [&[u32]; 6] = [
        &[TRIED],
        &[TO_PLAIN, TO_SWAPPED],
        &[TO_SWAPPED],
        &[TO_SWAPPED, TRIED],
        &[TO_PLAIN],
        &[TO_PLAIN, TRIED],
    ];
    for flags in forgeries {
        let world = world();
        let device = booted(&world);
        device.mark_staged_unchecked().unwrap();
        for &flag in flags {
            forge(&device, 1, flag);
        }
        for boot in 0..3 {
            let (version, state, _) = running(&device);
            assert_eq!(version, 1, "{flags:?}, boot {boot}");
            assert_ne!(state, BootState::Staged);
            assert!(device.dump(world.geometry.active) == world.old);
            assert!(!device.flash.borrow().swap_programmed(), "{flags:?}, boot {boot}");
            assert_eq!(device.floor(), 1);
        }
    }
}

#[test]
fn a_state_log_that_is_lost_means_nothing_in_flight() {
    let world = world();
    let device = booted(&world);
    stage_new(&world, &device);
    // The header rots: the log is gone, and with it the request. The device boots what it runs and starts a
    // new log; the staged image is still there for the next session.
    let state = device.bank_map().state;
    device.flash.borrow_mut().inject_ecc(state.offset as usize);
    assert_eq!(device.boot_state(), Ok(BootState::Confirmed));
    assert_eq!(running(&device), (1, BootState::Confirmed, None));
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    assert_eq!(plan.steps.len(), 0);
    device.apply(&plan.encode()).unwrap();
    assert_eq!(running(&device), (2, BootState::Trial, None));
}

/// The real chip: 2 MB in 128 KB sectors, 2 KB chunks, a 768 KB image per bank.
mod stm32h743zi {
    use super::*;

    fn world() -> World {
        World::with_geometry(STM32H743ZI, signer())
    }

    fn device(world: &World) -> Device<H7, Bank> {
        let device = Device::new(world.geometry, world.signer.trust(), PRODUCT);
        device.install(&world.old);
        let leftovers = bytes(9, world.geometry.dfu.len as usize);
        device
            .flash
            .borrow_mut()
            .load(world.geometry.dfu.offset as usize, &leftovers);
        device
    }

    #[test]
    fn updates_and_reverts() {
        let world = world();
        for app in [App::Good, App::Bad] {
            let device = device(&world);
            let seen = run_update(&device, &world, app, &[]);
            assert_eq!((seen.sessions, seen.trials), (1, 1));
            assert_eq!(device.flash.borrow().swapped(), app == App::Good);
        }
    }

    /// A cut at every flash operation of the update, torn one way each (more with `CHUNKY_SWEEP_SEEDS`).
    #[test]
    fn survives_a_cut_anywhere() {
        let world = world();
        let seeds = seeds() / 16;
        for app in [App::Good, App::Bad] {
            let quiet = device(&world);
            run_update(&quiet, &world, app, &[]);
            let operations = quiet.flash.borrow().ops();
            assert!(operations > 300, "{operations} operations");
            for cut in 0..operations {
                for seed in 0..seeds.max(1) {
                    run_update(&device(&world), &world, app, &[(cut, seed * 1_000_003 + cut)]);
                }
            }
        }
    }
}
