//! Shared by the integration tests: a release pair, a device, and the update loop a real device runs.
#![allow(dead_code)]

use chunky::geometry::{FlashGeometry, Region};
use chunky::{BootState, ImageDescriptor};
use chunky_server::{ImageMeta, Manifest, MemStore, Policy, Release, Signer, build_image, plan};
use chunky_sim::{Backend, Device, FlashModel, Swap};

pub const PRODUCT: u32 = 0x434B_5931;

pub fn signer() -> Signer {
    Signer::from_seed(&[7; 32])
}

/// Deterministic junk.
pub fn bytes(seed: u64, len: usize) -> Vec<u8> {
    let mut state = seed.wrapping_mul(0x9E37_79B9_7F4A_7C15) | 1;
    (0..len)
        .map(|_| {
            state ^= state << 13;
            state ^= state >> 7;
            state ^= state << 17;
            (state >> 24) as u8
        })
        .collect()
}

/// An old and a new release for one geometry, and the store that holds their chunks.
pub struct World {
    pub geometry: FlashGeometry,
    pub signer: Signer,
    pub store: MemStore,
    pub old: Vec<u8>,
    pub new: Vec<u8>,
    pub old_release: Release,
    pub new_release: Release,
}

pub fn meta(version: u32, security_counter: u32) -> ImageMeta {
    ImageMeta {
        product_id: PRODUCT,
        version,
        security_counter,
    }
}

impl World {
    /// Version 1 and version 2 of a firmware that fills about three quarters of the partition. Version 2
    /// changes a chunk, moves a chunk and appends one; the rest is the same.
    pub fn new<M: FlashModel>(chunk: u32, blocks: u32, signer: Signer) -> Self {
        Self::on::<M, Swap>(chunk, blocks, signer)
    }

    /// The same for a device on backend `B`.
    pub fn on<M: FlashModel, B: Backend<M>>(chunk: u32, blocks: u32, signer: Signer) -> Self {
        Self::with_geometry(B::layout(chunk, blocks), signer)
    }

    /// The same for a given layout, a chip preset for one.
    pub fn with_geometry(geometry: FlashGeometry, signer: Signer) -> Self {
        let chunk = geometry.chunk as usize;
        let chunks = geometry.chunks() as usize;
        let used = (chunks * 3 / 4).max(2);
        let old_payload = bytes(1, used * chunk);
        let mut new_payload = old_payload.clone();
        new_payload[..chunk].copy_from_slice(&bytes(2, chunk));
        if used > 3 {
            // Chunk 1 of the old image shows up as chunk 2 of the new one.
            let moved = old_payload[chunk..2 * chunk].to_vec();
            new_payload[2 * chunk..3 * chunk].copy_from_slice(&moved);
        }
        if used < chunks - 1 {
            new_payload.extend_from_slice(&bytes(3, chunk));
        }
        let old = build_image(&old_payload, &geometry, &meta(1, 1), &signer).unwrap();
        let new = build_image(&new_payload, &geometry, &meta(2, 2), &signer).unwrap();
        let mut store = MemStore::default();
        let old_release = Release::add(&old, &geometry, &mut store).unwrap();
        let new_release = Release::add(&new, &geometry, &mut store).unwrap();
        Self {
            geometry,
            signer,
            store,
            old,
            new,
            old_release,
            new_release,
        }
    }

    /// A device running the old release. Its DFU holds what a real one would: leftovers of an earlier image.
    pub fn device<M: FlashModel>(&self) -> Device<M> {
        self.device_on::<M, Swap>()
    }

    /// The same on backend `B`.
    pub fn device_on<M: FlashModel, B: Backend<M>>(&self) -> Device<M, B> {
        let device = Device::new(self.geometry, self.signer.trust(), PRODUCT);
        device.install(&self.old);
        let leftovers = bytes(9, self.geometry.dfu.len as usize);
        device
            .flash
            .borrow_mut()
            .load(self.geometry.dfu.offset as usize, &leftovers);
        device
    }

    /// The plan the server sends for this manifest.
    pub fn plan_for(&self, manifest: &[u8], release: &Release) -> chunky_server::Plan {
        let manifest = Manifest::parse(manifest).expect("the device sends a well-formed manifest");
        plan(&self.geometry, &manifest, release, &self.store, &Policy::default()).expect("a plan")
    }
}

/// How the new image behaves on its trial boot.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum App {
    /// It works and confirms.
    Good,
    /// It hangs: the watchdog resets it before it confirms.
    Bad,
}

/// What an update run saw.
#[derive(Clone, Copy, Debug, Default)]
pub struct Seen {
    pub boots: u32,
    pub sessions: u32,
    pub trials: u32,
    pub wire_bytes: usize,
}

/// Runs the device until the update to the new release is settled: it reboots, hashes, asks for a plan, applies
/// it, reboots, tries, confirms or reverts, and starts over after every power cut. `cuts` are armed one after
/// another, each `(operations from now, seed)`.
///
/// Checked at every boot: the bootloader always finds something to run, ACTIVE is byte for byte the old or the
/// new release, and the floor never goes down or past the newest confirmed release.
///
/// The loop knows nothing about the backend: a device on any [`Backend`] runs through it.
pub fn run_update<M: FlashModel, B: Backend<M>>(
    device: &Device<M, B>,
    world: &World,
    app: App,
    cuts: &[(u64, u64)],
) -> Seen {
    let mut cuts = cuts.iter();
    let mut seen = Seen::default();
    let mut last_floor = 0;
    let arm = |cuts: &mut std::slice::Iter<(u64, u64)>| {
        if let Some(&(after, seed)) = cuts.next() {
            device.flash.borrow_mut().cut_after(after, seed);
        }
    };
    arm(&mut cuts);
    loop {
        if device.flash.borrow().is_dead() {
            device.flash.borrow_mut().power_on();
            arm(&mut cuts);
        }
        seen.boots += 1;
        assert!(seen.boots < 60, "the update does not settle: {seen:?}");
        let dead = || device.flash.borrow().is_dead();

        let outcome = match device.boot() {
            Ok(outcome) => outcome,
            Err(_) => {
                assert!(dead(), "the bootloader failed without a power cut");
                continue;
            }
        };
        let Some((image, state)) = B::running(&outcome) else {
            panic!("the bootloader found nothing to run: {outcome:?} after {seen:?}");
        };
        let active = device.dump(world.geometry.active);
        let running_new = active == world.new;
        assert!(
            running_new || active == world.old,
            "ACTIVE is neither release after {seen:?}"
        );
        assert_eq!(image.version, if running_new { 2 } else { 1 });

        let floor = device.floor();
        assert!(floor >= last_floor, "the floor went down: {last_floor} -> {floor}");
        assert!(
            floor <= if app == App::Good { 2 } else { 1 },
            "the floor is past any confirmed release: {floor}"
        );
        last_floor = floor;

        if state == BootState::Trial {
            assert!(running_new, "the old release is on trial");
            seen.trials += 1;
            match app {
                App::Bad => continue,
                App::Good => {
                    if device.confirm().is_err() {
                        assert!(dead(), "confirm failed without a power cut");
                        continue;
                    }
                }
            }
        }
        if running_new {
            assert_eq!(app, App::Good, "a release that never confirmed stayed");
            break;
        }
        if app == App::Bad && seen.trials > 0 {
            // The server has heard that version 2 failed its trial here and stops offering it.
            break;
        }

        seen.sessions += 1;
        let manifest = match device.manifest() {
            Ok(manifest) => manifest,
            Err(_) => {
                assert!(dead(), "hashing failed without a power cut");
                continue;
            }
        };
        let plan = world.plan_for(&manifest, &world.new_release);
        seen.wire_bytes += plan.stats.wire_bytes;
        if let Err(error) = device.apply(&plan.encode()) {
            assert!(dead(), "the plan failed without a power cut: {error:?}");
        }
    }

    // Settled. One more quiet boot: same image, and the floor is where it belongs.
    device.flash.borrow_mut().disarm();
    device.flash.borrow_mut().power_on();
    let expected: (&[u8], u32) = match app {
        App::Good => (&world.new, 2),
        App::Bad => (&world.old, 1),
    };
    let settled = device.boot();
    match settled.as_ref().map(B::running) {
        Ok(Some((image, state))) => {
            assert_ne!(state, BootState::Trial);
            assert_eq!(image.version, expected.1);
        }
        _ => panic!("the settled device does not boot: {settled:?}"),
    }
    assert!(device.dump(world.geometry.active) == expected.0);
    assert_eq!(device.floor(), expected.1);
    seen
}

/// Runs the update once without cuts to count the flash operations, then once per operation with power cut
/// there, for each of `seeds` ways of tearing it.
pub fn sweep<M: FlashModel, B: Backend<M>>(chunk: u32, blocks: u32, app: App, seeds: u64) {
    sweep_from::<M, B>(chunk, blocks, app, seeds, |_| {});
}

/// The same, with every device first brought into the state `prepare` leaves it in.
pub fn sweep_from<M: FlashModel, B: Backend<M>>(
    chunk: u32,
    blocks: u32,
    app: App,
    seeds: u64,
    prepare: impl Fn(&Device<M, B>),
) {
    let world = World::on::<M, B>(chunk, blocks, signer());
    let device = world.device_on::<M, B>();
    prepare(&device);
    let start = device.flash.borrow().ops();
    let quiet = run_update(&device, &world, app, &[]);
    assert_eq!(quiet.sessions, 1);
    assert_eq!(quiet.trials, 1);
    let operations = device.flash.borrow().ops() - start;
    assert!(
        operations > 20,
        "the run is suspiciously short: {operations} operations"
    );
    for cut in 0..operations {
        for seed in 0..seeds {
            let device = world.device_on::<M, B>();
            prepare(&device);
            run_update(&device, &world, app, &[(cut, seed * 1_000_003 + cut)]);
        }
    }
}

/// The descriptor of an image.
pub fn descriptor_of(image: &[u8]) -> ImageDescriptor {
    chunky_server::image::read_descriptor_unverified(image.last_chunk().unwrap()).unwrap()
}

/// The regions a plan must never change.
pub fn protected(geometry: &FlashGeometry) -> [Region; 4] {
    let spare = Region {
        offset: geometry.dfu.offset + geometry.active.len,
        len: geometry.dfu.len - geometry.active.len,
    };
    [geometry.state, geometry.floor, geometry.active, spare]
}
