//! What a hostile server, a hostile plan, a compromised application and a torn flash word can and cannot do.

mod common;

use chunky::descriptor::{ImageError, Trust};
use chunky::plan::{PLAN_HEADER_LEN, Step};
use chunky::{ApplyError, BootState, ChunkHash};
use chunky_server::{Manifest, MemStore, PlanningError, Policy, Release, Signer, Warning, build_image, plan};
use chunky_sim::{Device, F1, G0, H7Mini, layout};
use chunky_swap::BootOutcome;
use common::{App, PRODUCT, World, bytes, meta, protected, run_update, signer};
use embedded_storage::nor_flash::NorFlash;

type Model = G0;

fn world() -> World {
    World::new::<Model>(2048, 6, signer())
}

/// A device that has booted once, so its floor is that of the old release.
fn booted(world: &World) -> Device<Model> {
    let device = world.device::<Model>();
    assert!(matches!(
        device.boot(),
        Ok(BootOutcome::Run {
            state: BootState::Confirmed,
            ..
        })
    ));
    assert_eq!(device.floor(), 1);
    device
}

/// Applies a plan for `image` and expects it to be refused before any flash operation.
fn refused_untouched(world: &World, device: &Device<Model>, image: &[u8]) -> ApplyError {
    let mut store = MemStore::default();
    let target = Release::add(image, &world.geometry, &mut store).unwrap();
    let mut manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    // The planner would refuse some of these itself; the device must not rely on that.
    manifest.header.scheme = target.info().scheme as u8;
    let plan = plan(&world.geometry, &manifest, &target, &store, &Policy::default()).unwrap();
    let before = device.flash.borrow().ops();
    let error = device.apply(&plan.encode()).unwrap_err();
    assert_eq!(
        device.flash.borrow().ops(),
        before,
        "the flash was touched before the plan was refused"
    );
    error
}

#[test]
fn image_signed_with_another_key_is_refused() {
    let world = world();
    let device = booted(&world);
    let forged = build_image(
        &bytes(5, 4096),
        &world.geometry,
        &meta(2, 2),
        &Signer::from_seed(&[8; 32]),
    )
    .unwrap();
    assert_eq!(
        refused_untouched(&world, &device, &forged),
        ApplyError::Image(ImageError::BadSignature)
    );
}

#[test]
fn tampered_descriptor_is_refused() {
    let world = world();
    let device = booted(&world);
    let mut tampered = world.new.clone();
    let version = tampered.len() - 128 + 12;
    tampered[version] = 9;
    assert_eq!(
        refused_untouched(&world, &device, &tampered),
        ApplyError::Image(ImageError::BadSignature)
    );
}

#[test]
fn image_for_another_product_is_refused() {
    let world = world();
    let device = booted(&world);
    let mut other = meta(2, 2);
    other.product_id ^= 1;
    let image = build_image(&bytes(5, 4096), &world.geometry, &other, &world.signer).unwrap();
    assert_eq!(
        refused_untouched(&world, &device, &image),
        ApplyError::Image(ImageError::WrongProduct)
    );
}

#[test]
fn unsigned_image_is_refused_by_a_signed_device() {
    let world = world();
    let device = booted(&world);
    let image = build_image(&bytes(5, 4096), &world.geometry, &meta(2, 2), &Signer::Unsigned).unwrap();
    assert_eq!(
        refused_untouched(&world, &device, &image),
        ApplyError::Image(ImageError::WrongScheme)
    );
}

#[test]
fn rollback_below_the_floor_is_refused_everywhere() {
    let world = world();
    let device = world.device::<Model>();
    run_update(&device, &world, App::Good, &[]);
    assert_eq!(device.floor(), 2);

    // Through the front door: a plan back to the old release.
    assert_eq!(
        refused_untouched(&world, &device, &world.old),
        ApplyError::Image(ImageError::RolledBack)
    );

    // Through the back door: a compromised application puts the old, validly signed release into DFU and asks
    // for a swap. The bootloader refuses before moving a page.
    device
        .flash
        .borrow_mut()
        .load(world.geometry.dfu.offset as usize, &world.old);
    device.mark_staged_unchecked().unwrap();
    assert_eq!(device.boot_state(), Ok(BootState::Staged));
    let outcome = device.boot().unwrap();
    let BootOutcome::Run { image, state, rejected } = outcome else {
        panic!("{outcome:?}")
    };
    assert_eq!(
        (image.version, state, rejected),
        (2, BootState::Confirmed, Some(ImageError::RolledBack))
    );
    assert!(device.dump(world.geometry.active) == world.new);
}

#[test]
fn downgrade_within_the_same_security_counter_is_allowed() {
    let geometry = layout::<Model>(2048, 6);
    let signer = signer();
    let newer = build_image(&bytes(1, 6000), &geometry, &meta(5, 3), &signer).unwrap();
    let older = build_image(&bytes(2, 6000), &geometry, &meta(4, 3), &signer).unwrap();
    let mut store = MemStore::default();
    let target = Release::add(&older, &geometry, &mut store).unwrap();
    let device = Device::<Model>::new(geometry, signer.trust(), PRODUCT);
    device.install(&newer);
    device.boot().unwrap();
    let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    let plan = plan(&geometry, &manifest, &target, &store, &Policy::default()).unwrap();
    assert_eq!(device.apply(&plan.encode()).unwrap().version, 4);
}

#[test]
fn junk_in_dfu_with_a_forged_swap_request_is_not_booted() {
    let world = world();
    let device = booted(&world);
    device.mark_staged_unchecked().unwrap();
    let outcome = device.boot().unwrap();
    let BootOutcome::Run { image, state, rejected } = outcome else {
        panic!("{outcome:?}")
    };
    assert_eq!((image.version, state), (1, BootState::Confirmed));
    assert!(rejected.is_some());
    assert!(device.dump(world.geometry.active) == world.old);
}

#[test]
fn forged_swap_progress_never_runs_unverified_code() {
    // A compromised application writes "swap" and marks the first step done, so the bootloader takes DFU for
    // a swap in progress and cannot verify it first. Junk reaches ACTIVE, and is caught there.
    let world = world();
    let device = booted(&world);
    device.mark_staged_unchecked().unwrap();
    device.part(world.geometry.state).write(16, &[0; 8]).unwrap();
    assert_eq!(device.boot_state(), Ok(BootState::Trial));
    match device.boot().unwrap() {
        BootOutcome::Run { image, .. } => {
            assert_eq!(image.version, 1);
            assert!(device.dump(world.geometry.active) == world.old);
        }
        BootOutcome::Recovery { .. } => {}
    }
}

#[test]
fn plan_for_another_geometry_is_refused_on_both_sides() {
    let world = world();
    let device = booted(&world);
    let other = World::new::<Model>(2048, 5, signer());

    // Server side.
    let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    let refused = plan(
        &other.geometry,
        &manifest,
        &other.new_release,
        &other.store,
        &Policy::default(),
    );
    assert!(matches!(refused, Err(PlanningError::GeometryMismatch)));

    // Device side: a plan made for the other layout.
    let other_device = other.device::<Model>();
    let foreign = other
        .plan_for(&other_device.manifest().unwrap(), &other.new_release)
        .encode();
    let before = device.flash.borrow().ops();
    assert_eq!(device.apply(&foreign).unwrap_err(), ApplyError::GeometryMismatch);
    assert_eq!(device.flash.borrow().ops(), before);
}

/// A real plan header followed by the steps a test wants.
fn crafted(world: &World, device: &Device<Model>, steps: &[(Step, Option<Vec<u8>>)]) -> Vec<u8> {
    let mut real = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    real.header.step_count = steps.len() as u32;
    real.steps = steps.to_vec();
    real.encode()
}

#[test]
fn hostile_steps_are_stopped_and_touch_nothing_but_dfu() {
    let world = world();
    let chunk = world.geometry.chunk as usize;
    let chunks = world.geometry.chunks() as u16;
    let data = bytes(42, chunk);
    let hash = ChunkHash::of(&data);
    let write = |dst| (Step::Write { dst, hash }, Some(data.clone()));
    let erase = |block| (Step::Erase { block }, None);
    let active_hash = ChunkHash::of(&world.old[..chunk]);

    type Steps = Vec<(Step, Option<Vec<u8>>)>;
    let cases: Vec<(&str, Steps, ApplyError)> = vec![
        ("write past the image", vec![write(chunks)], ApplyError::OutOfRange),
        ("erase past the image", vec![erase(chunks)], ApplyError::OutOfRange),
        (
            "copy from past the image",
            vec![(
                Step::Copy {
                    dst: 0,
                    src: chunks,
                    hash,
                },
                None,
            )],
            ApplyError::OutOfRange,
        ),
        (
            "write into a chunk that is not blank",
            vec![write(0)],
            ApplyError::NotBlank,
        ),
        (
            "write the same chunk twice",
            vec![erase(0), write(0), write(0)],
            ApplyError::OutOfOrder,
        ),
        (
            "erase a block after writing into it",
            vec![erase(0), write(0), erase(0)],
            ApplyError::OutOfOrder,
        ),
        (
            "go backwards",
            vec![erase(1), write(1), erase(0)],
            ApplyError::OutOfOrder,
        ),
        (
            "data that does not match its hash",
            vec![erase(0), (Step::Write { dst: 0, hash }, Some(bytes(43, chunk)))],
            ApplyError::DataMismatch,
        ),
        (
            "copy of a source that is not what the step says",
            vec![
                erase(0),
                (
                    Step::Copy {
                        dst: 0,
                        src: 1,
                        hash: active_hash,
                    },
                    None,
                ),
            ],
            ApplyError::SourceMismatch,
        ),
        (
            "a plan that stops short of the image",
            vec![erase(0), write(0)],
            ApplyError::Image(ImageError::BadMagic),
        ),
    ];
    for (name, steps, expected) in cases {
        let device = booted(&world);
        let before: Vec<_> = protected(&world.geometry)
            .iter()
            .map(|&region| device.dump(region))
            .collect();
        let error = device.apply(&crafted(&world, &device, &steps)).unwrap_err();
        assert_eq!(error, expected, "{name}");
        let after: Vec<_> = protected(&world.geometry)
            .iter()
            .map(|&region| device.dump(region))
            .collect();
        assert!(before == after, "{name}: something outside DFU changed");
        assert_eq!(
            device.boot_state(),
            Ok(BootState::Confirmed),
            "{name}: a swap was requested"
        );
        assert!(
            matches!(
                device.boot(),
                Ok(BootOutcome::Run {
                    state: BootState::Confirmed,
                    ..
                })
            ),
            "{name}"
        );
    }
}

#[test]
fn more_steps_than_announced_or_possible_are_refused() {
    let world = world();
    let device = booted(&world);
    let mut plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);

    let mut longer = plan.encode();
    longer.extend_from_slice(&Step::Erase { block: 0 }.encode());
    assert_eq!(device.apply(&longer).unwrap_err(), ApplyError::TooManySteps);

    plan.header.step_count = u32::MAX;
    let before = booted(&world);
    let ops = before.flash.borrow().ops();
    assert_eq!(before.apply(&plan.encode()).unwrap_err(), ApplyError::TooManySteps);
    assert_eq!(before.flash.borrow().ops(), ops);

    assert_eq!(
        device.apply(&longer[..PLAN_HEADER_LEN - 1]).unwrap_err(),
        ApplyError::Incomplete
    );
}

#[test]
fn staging_is_refused_while_an_image_is_on_trial() {
    let world = world();
    let device = booted(&world);
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release).encode();
    device.apply(&plan).unwrap();
    assert_eq!(device.boot_state(), Ok(BootState::Staged));
    assert_eq!(device.apply(&plan).unwrap_err(), ApplyError::NotConfirmed);

    assert!(matches!(
        device.boot(),
        Ok(BootOutcome::Run {
            state: BootState::Trial,
            ..
        })
    ));
    // DFU now holds the image to revert to: nothing may write it.
    let back = world.plan_for(&device.manifest().unwrap(), &world.new_release).encode();
    let ops = device.flash.borrow().ops();
    assert_eq!(device.apply(&back).unwrap_err(), ApplyError::NotConfirmed);
    assert_eq!(device.flash.borrow().ops(), ops);
}

#[test]
fn unsigned_devices_update_and_the_planner_says_so() {
    let world = World::new::<F1>(1024, 8, Signer::Unsigned);
    let device = world.device::<F1>();
    assert_eq!(device.trust, Trust::Unsigned);
    device.boot().unwrap();
    let first = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    assert_eq!(first.warnings, vec![Warning::UnsignedDevice]);

    let device = world.device::<F1>();
    run_update(&device, &world, App::Good, &[]);
    assert_eq!(device.floor(), 2);

    // The floor still holds without signatures.
    let manifest = device.manifest().unwrap();
    let back = world.plan_for(&manifest, &world.old_release).encode();
    assert_eq!(
        device.apply(&back).unwrap_err(),
        ApplyError::Image(ImageError::RolledBack)
    );

    // And an unsigned descriptor that was altered is caught by its own hash.
    let mut altered = world.new.clone();
    let counter = altered.len() - 128 + 16;
    altered[counter] = 200;
    let mut store = MemStore::default();
    let target = Release::add(&altered, &world.geometry, &mut store).unwrap();
    let plan = plan(
        &world.geometry,
        &Manifest::parse(&manifest).unwrap(),
        &target,
        &store,
        &Policy::default(),
    );
    assert_eq!(
        device.apply(&plan.unwrap().encode()).unwrap_err(),
        ApplyError::Image(ImageError::BadSignature)
    );
}

#[test]
fn the_planner_refuses_a_release_sealed_the_other_way() {
    let world = world();
    let device = booted(&world);
    let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    assert!(
        plan(
            &world.geometry,
            &manifest,
            &world.new_release,
            &world.store,
            &Policy::default()
        )
        .unwrap()
        .warnings
        .is_empty()
    );
    let unsigned = build_image(&bytes(5, 4096), &world.geometry, &meta(2, 2), &Signer::Unsigned).unwrap();
    let mut store = MemStore::default();
    let target = Release::add(&unsigned, &world.geometry, &mut store).unwrap();
    let refused = plan(&world.geometry, &manifest, &target, &store, &Policy::default());
    assert!(matches!(refused, Err(PlanningError::SchemeMismatch)));
}

#[test]
fn a_faulting_word_in_dfu_is_reported_and_erased() {
    let world = World::new::<H7Mini>(2048, 3, signer());
    let device = world.device::<H7Mini>();
    device.boot().unwrap();
    let dfu = world.geometry.dfu.offset as usize;
    device.flash.borrow_mut().inject_ecc(dfu + 2048 + 64);
    let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    assert_eq!(manifest.dfu[1], chunky::ChunkState::Unreadable);
    run_update(&device, &world, App::Good, &[]);
}

#[test]
fn a_faulting_word_in_active_ends_in_recovery_not_in_a_fault() {
    let world = world();
    let device = booted(&world);
    device
        .flash
        .borrow_mut()
        .inject_ecc(world.geometry.active.offset as usize + 4096);
    let outcome = device.boot().unwrap();
    assert_eq!(
        outcome,
        BootOutcome::Recovery {
            reason: ImageError::Unreadable
        }
    );
}

#[test]
fn a_copy_source_that_started_faulting_stops_the_plan() {
    let world = world();
    let device = booted(&world);
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    let source = plan.steps.iter().find_map(|(step, _)| match step {
        Step::Copy { src, .. } => Some(*src as usize),
        _ => None,
    });
    let source = source.expect("the plan copies something");
    device
        .flash
        .borrow_mut()
        .inject_ecc(world.geometry.active.offset as usize + source * 2048);
    assert_eq!(device.apply(&plan.encode()).unwrap_err(), ApplyError::SourceUnreadable);
}

#[test]
fn faulting_words_in_the_boot_state_do_not_stop_the_boot() {
    let world = world();
    for word in 0..6 {
        let device = booted(&world);
        device
            .flash
            .borrow_mut()
            .inject_ecc(world.geometry.state.offset as usize + word * 8);
        let outcome = device.boot().unwrap();
        assert!(
            matches!(outcome, BootOutcome::Run { image, .. } if image.version == 1),
            "word {word}: {outcome:?}"
        );
        run_update(&device, &world, App::Good, &[]);
    }
}

#[test]
fn one_flipped_bit_does_not_raise_the_floor() {
    let world = world();
    let device = booted(&world);
    // Bit rot in the first unused unit of the floor counter.
    let floor = world.geometry.floor.offset as usize;
    let mut unit = device.flash.borrow().dump(floor + 8, 8);
    unit[3] &= !0x10;
    device.flash.borrow_mut().load(floor + 8, &unit);
    assert_eq!(device.floor(), 1);
    assert!(matches!(device.boot(), Ok(BootOutcome::Run { .. })));
    run_update(&device, &world, App::Good, &[]);
    assert_eq!(device.floor(), 2);
}
