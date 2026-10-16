//! The planner and the parsers, against arbitrary device states and arbitrary bytes.

mod common;

use chunky::manifest::ChunkState;
use chunky::plan::{PLAN_HEADER_LEN, Step, read_header_fixed, read_step, step_len};
use chunky_server::image::read_descriptor_unverified;
use chunky_server::planner::PlanStep;
use chunky_server::store::BlobStore;
use chunky_server::{DirStore, Manifest, Policy, plan};
use chunky_sim::{Device, FlashModel, G0, H7Mini};
use common::{World, bytes, protected, signer};
use proptest::prelude::*;

/// What a DFU chunk holds before the update.
#[derive(Clone, Copy, Debug)]
enum Before {
    Target,
    Old,
    Blank,
    Junk,
    Faulting,
}

fn before() -> impl Strategy<Value = Before> {
    prop_oneof![
        Just(Before::Target),
        Just(Before::Old),
        Just(Before::Blank),
        Just(Before::Junk),
        Just(Before::Faulting)
    ]
}

/// A device whose DFU chunks are in the given states.
fn device_with<M: FlashModel>(world: &World, dfu: &[Before]) -> Device<M> {
    let device = world.device::<M>();
    let chunk = world.geometry.chunk as usize;
    let base = world.geometry.dfu.offset as usize;
    for (index, state) in dfu.iter().enumerate() {
        let range = index * chunk..(index + 1) * chunk;
        let content = match state {
            Before::Target => world.new[range].to_vec(),
            Before::Old => world.old[range].to_vec(),
            Before::Blank => vec![0xFF; chunk],
            Before::Junk | Before::Faulting => bytes(100 + index as u64, chunk),
        };
        device.flash.borrow_mut().load(base + index * chunk, &content);
        if matches!(state, Before::Faulting) {
            device.flash.borrow_mut().inject_ecc(base + index * chunk + 32);
        }
    }
    device.boot().unwrap();
    device
}

fn plan_reaches_the_target<M: FlashModel>(chunk: u32, blocks: u32, dfu: &[Before], trust_blank: Option<bool>) {
    let world = World::new::<M>(chunk, blocks, signer());
    let device = device_with::<M>(&world, dfu);
    let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
    let plan = plan(
        &world.geometry,
        &manifest,
        &world.new_release,
        &world.store,
        &Policy { trust_blank },
    )
    .unwrap();

    let image = device.apply(&plan.encode()).expect("the device accepts the plan");
    assert_eq!(image.version, 2);
    let staged = device.dump(world.geometry.dfu);
    assert!(staged[..world.new.len()] == world.new[..]);

    // Only chunks the running image does not hold travel.
    let in_active = |state: &ChunkState| manifest.active.contains(state);
    let must_travel = world
        .new_release
        .chunks
        .iter()
        .filter(|c| **c != ChunkState::Blank && !in_active(c))
        .count();
    assert!(plan.stats.written as usize <= must_travel);
    assert_eq!(plan.stats.wire_bytes, plan.encode().len());

    // Planning again finds nothing left to do.
    let again = Manifest::parse(&device.manifest().unwrap()).unwrap();
    assert!(again.dfu == world.new_release.chunks);
}

proptest! {
    #![proptest_config(ProptestConfig::with_cases(200))]

    #[test]
    fn any_dfu_state_reaches_the_target_g0(dfu in prop::collection::vec(before(), 6), trust in any::<Option<bool>>()) {
        plan_reaches_the_target::<G0>(2048, 6, &dfu, trust);
    }

    #[test]
    fn any_dfu_state_reaches_the_target_small_chunks(
        dfu in prop::collection::vec(before(), 12),
        trust in any::<Option<bool>>(),
    ) {
        plan_reaches_the_target::<H7Mini>(2048, 3, &dfu, trust);
    }

    #[test]
    fn parsers_take_any_bytes(raw in prop::collection::vec(any::<u8>(), 0..400)) {
        // One encoding each: what a reader accepts, written again, is the bytes it read.
        if let Ok(manifest) = Manifest::parse(&raw) {
            prop_assert_eq!(&manifest.encode(), &raw);
        }
        if let Some(header) = raw.first_chunk::<PLAN_HEADER_LEN>()
            && let Ok(parsed) = read_header_fixed(header)
        {
            let mut again = [0; PLAN_HEADER_LEN];
            parsed.to_fixed(&mut again).unwrap();
            prop_assert_eq!(&again, header);
        }
        if let Ok(view) = read_step(&raw) {
            let len = step_len(&view);
            let step = Step::from_head_tail(&raw[..len]).unwrap();
            let mut again = vec![0; step.head_tail_len()];
            step.to_head_tail(&mut again).unwrap();
            prop_assert_eq!(&again[..], &raw[..len]);
        }
        if let Some(descriptor) = raw.first_chunk::<128>()
            && let Ok(body) = read_descriptor_unverified(descriptor)
        {
            let mut again = [0; 64];
            body.to_fixed(&mut again).unwrap();
            prop_assert_eq!(&again[..], &descriptor[..64]);
        }
    }

    /// A real header, then whatever: the device never panics, never requests a swap, never writes outside DFU.
    #[test]
    fn arbitrary_steps_stay_inside_dfu(
        steps in prop::collection::vec((1u8..5, 0u16..14, 0u16..14, any::<bool>(), any::<u64>()), 0..12),
    ) {
        let world = World::new::<H7Mini>(2048, 3, signer());
        let device = world.device::<H7Mini>();
        device.boot().unwrap();
        let chunk = world.geometry.chunk as usize;
        let mut wire = world.plan_for(&device.manifest().unwrap(), &world.new_release);
        let mut body = Vec::new();
        let count = steps.len() as u32;
        for (op, dst, src, honest, seed) in steps {
            let data = bytes(seed, chunk);
            let source = &world.old[(src as usize % 12) * chunk..][..chunk];
            let hash = match (op, honest) {
                (3, true) => chunky::ChunkHash::of(&data),
                (2, true) => chunky::ChunkHash::of(source),
                _ => chunky::ChunkHash([seed as u8; 8]),
            };
            let step = match op {
                1 => PlanStep::Erase { block: dst },
                2 => PlanStep::Copy { dst, src, hash },
                3 => PlanStep::Write { dst, hash, data },
                _ => {
                    // an op no format has
                    body.extend_from_slice(&[op, seed as u8, (seed >> 8) as u8]);
                    continue;
                }
            };
            body.extend_from_slice(&step.encode());
        }
        wire.header.step_count = count;
        wire.header.body_len = body.len() as u32;
        let mut raw = wire.encode()[..PLAN_HEADER_LEN].to_vec();
        raw.extend_from_slice(&body);
        let before: Vec<_> = protected(&world.geometry).iter().map(|&region| device.dump(region)).collect();
        prop_assert!(device.apply(&raw).is_err());
        let after: Vec<_> = protected(&world.geometry).iter().map(|&region| device.dump(region)).collect();
        prop_assert!(before == after);
        prop_assert_eq!(device.boot_state(), Ok(chunky::BootState::Confirmed));
    }
}

#[test]
fn an_unchanged_image_costs_no_chunks_on_the_wire() {
    let world = World::new::<G0>(2048, 6, signer());
    let device = world.device::<G0>();
    device.boot().unwrap();
    let plan = world.plan_for(&device.manifest().unwrap(), &world.old_release);
    assert_eq!(plan.stats.written, 0);
    assert!(plan.stats.copied > 0);
    assert!(plan.stats.wire_bytes < world.geometry.chunk as usize);
}

#[test]
fn a_delta_sends_only_new_chunks() {
    let world = World::new::<G0>(2048, 6, signer());
    let device = world.device::<G0>();
    device.boot().unwrap();
    let plan = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    // Version 2 has one changed chunk, one appended chunk and a new descriptor chunk; the moved chunk is copied.
    assert_eq!(plan.stats.written, 3);
    assert!(
        plan.steps
            .iter()
            .any(|step| matches!(step, PlanStep::Copy { dst: 2, src: 1, .. }))
    );
    assert!(plan.stats.wire_bytes < plan.stats.image_bytes);
}

#[test]
fn a_cut_staging_resumes_with_what_is_left() {
    let world = World::new::<H7Mini>(2048, 3, signer());
    let device = world.device::<H7Mini>();
    device.boot().unwrap();
    let first = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    let steps = first.steps.len() as u64;
    device.flash.borrow_mut().cut_after(steps - 2, 1);
    assert!(device.apply(&first.encode()).is_err());
    device.flash.borrow_mut().power_on();
    device.boot().unwrap();

    let second = world.plan_for(&device.manifest().unwrap(), &world.new_release);
    assert!(second.stats.erased_blocks < first.stats.erased_blocks);
    assert!(second.steps.len() < first.steps.len());
    device.apply(&second.encode()).unwrap();
}

#[test]
fn the_directory_store_keeps_and_checks_blobs() {
    let root = std::env::temp_dir().join(format!("chunky-store-test-{}", std::process::id()));
    let mut store = DirStore::open(&root).unwrap();
    let data = bytes(7, 2048);
    let hash = store.put(&data).unwrap();
    assert_eq!(store.put(&data).unwrap(), hash);
    assert_eq!(store.get(&hash).unwrap(), Some(data));
    assert_eq!(store.get(&chunky::ChunkHash([0; 8])).unwrap(), None);

    // A blob that rotted on disk is not handed out.
    let name: String = hash.0.iter().map(|b| format!("{b:02x}")).collect();
    std::fs::write(root.join(&name[..2]).join(&name), b"rot").unwrap();
    assert!(store.get(&hash).is_err());
    std::fs::remove_dir_all(&root).unwrap();
}
