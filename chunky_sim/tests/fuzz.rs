//! Parser fuzzing at scale (SIM-3): garbage and mutated real input into every untrusted parser of format 2, and
//! arbitrary and mutated real plans, with power cuts inside them, into the apply engine on simulated flash.
//!
//! `just fuzz` runs it with a large case count (`CHUNKY_FUZZ_CASES`); `just test` runs the same properties with
//! a small one. What must hold, whatever the bytes:
//!
//! - a parser never panics, and a mutated sealed descriptor is never accepted;
//! - the apply engine never writes outside DFU[0..image_len) (the state region aside), never programs a unit
//!   that is not blank (the simulator panics on it), and the device is never staged unless DFU holds a release;
//! - after any number of cuts inside any mutated plans, the device boots a verified release and an honest
//!   update then settles.

mod common;

use chunky::descriptor::{Expect, Trust, open_descriptor, split_descriptor, unsigned_seal};
use chunky::hash::ChunkHash;
use chunky::plan::{PLAN_HEADER_LEN, read_header_fixed, read_step, step_len, step_len_from_head};
use chunky::{ApplyError, BootState};
use chunky_format::{ChunkEntry, DESCRIPTOR_DOMAIN, ManifestHeader, SEAL_LEN, SignedDescriptor, read_header_sw};
use chunky_server::planner::PlanStep;
use chunky_server::{Manifest, Signer};
use chunky_sim::{Backend, Bank, Device, F1, FlashModel, G0, H7Mini, Rp2350, Swap};
use common::{App, PRODUCT, World, bytes, run_update, signer};
use proptest::prelude::*;

/// Cases per property: `CHUNKY_FUZZ_CASES` when set, else `default` (small, so that `just test` stays quick).
fn cases(default: u32) -> ProptestConfig {
    let cases = std::env::var("CHUNKY_FUZZ_CASES")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(default);
    ProptestConfig::with_cases(cases)
}

fn junk(max: usize) -> impl Strategy<Value = Vec<u8>> {
    prop::collection::vec(any::<u8>(), 0..max)
}

/// Bytes biased toward something that looks like it: a prefix of a real encoding, then junk.
fn lookalike(real: Vec<u8>, junk_max: usize) -> impl Strategy<Value = Vec<u8>> {
    let len = real.len();
    (0..=len, junk(junk_max)).prop_map(move |(keep, tail)| {
        let mut out = real[..keep].to_vec();
        out.extend(tail);
        out
    })
}

fn expect_for(world: &World) -> Expect {
    Expect {
        product_id: PRODUCT,
        geometry_id: world.geometry.id(),
        image_len: world.geometry.image_len(),
        floor: 0,
    }
}

// ---------------------------------------------------------------------------------------------------------
// Parsers
// ---------------------------------------------------------------------------------------------------------

struct Descriptors {
    expect: Expect,
    signed: (Trust, [u8; 128]),
    unsigned: (Trust, [u8; 128]),
}

fn descriptors() -> Descriptors {
    let world = World::new::<G0>(2048, 4, signer());
    let unsigned_world = World::new::<G0>(2048, 4, Signer::Unsigned);
    let last = |image: &[u8]| *image.last_chunk::<128>().unwrap();
    Descriptors {
        expect: expect_for(&world),
        signed: (world.signer.trust(), last(&world.new)),
        unsigned: (Trust::Unsigned, last(&unsigned_world.new)),
    }
}

fn trusts() -> [Trust; 2] {
    [signer().trust(), Trust::Unsigned]
}

fn signed(raw: &[u8; 128]) -> SignedDescriptor {
    split_descriptor(raw)
}

proptest! {
    #![proptest_config(cases(2000))]

    /// Any 128 bytes: opening never panics, with either trust.
    #[test]
    fn descriptor_open_on_garbage(raw in prop::collection::vec(any::<u8>(), 128)) {
        let raw: [u8; 128] = raw.try_into().unwrap();
        let d = descriptors();
        for trust in trusts() {
            let _ = open_descriptor(&signed(&raw), &trust, &d.expect);
        }
    }

    /// Garbage that carries a valid unsigned seal reaches the decoder of the body: no panic there either, and
    /// whatever is accepted names the unsigned scheme.
    #[test]
    fn descriptor_decode_of_sealed_garbage(body in prop::collection::vec(any::<u8>(), 64), magic in any::<bool>()) {
        let mut body: [u8; 64] = body.try_into().unwrap();
        if magic {
            body[..4].copy_from_slice(&chunky_format::DESCRIPTOR_MAGIC);
            body[4] = chunky_format::FORMAT;
        }
        let seal = unsigned_seal(DESCRIPTOR_DOMAIN, &body);
        let d = descriptors();
        let raw = [body.as_slice(), seal.as_slice()].concat();
        let raw: [u8; 128] = raw.try_into().unwrap();
        for trust in trusts() {
            let result = open_descriptor(&signed(&raw), &trust, &d.expect);
            if matches!(trust, Trust::Ed25519(_)) {
                prop_assert!(result.is_err(), "junk with an unsigned seal was accepted by a key");
            }
        }
    }

    /// A real descriptor with any bytes changed is never accepted by the trust that sealed it.
    #[test]
    fn mutated_descriptor_is_never_accepted(
        which in any::<bool>(),
        edits in prop::collection::vec((0..128usize, 1..=255u8), 1..6),
    ) {
        let d = descriptors();
        let (trust, real) = if which { d.signed } else { d.unsigned };
        prop_assert_eq!(open_descriptor(&signed(&real), &trust, &d.expect).map(|_| ()), Ok(()));
        let mut raw = real;
        for (at, xor) in edits {
            raw[at] ^= xor;
        }
        if raw != real {
            prop_assert!(open_descriptor(&signed(&raw), &trust, &d.expect).is_err());
        }
    }

    /// Seal bytes only: a signature that is not the signature.
    #[test]
    fn descriptor_with_a_junk_seal(seal in prop::collection::vec(any::<u8>(), SEAL_LEN)) {
        let d = descriptors();
        let mut raw = d.signed.1;
        raw[64..].copy_from_slice(&seal);
        if raw != d.signed.1 {
            prop_assert!(open_descriptor(&signed(&raw), &d.signed.0, &d.expect).is_err());
        }
    }

    /// The plan header, any length, both readers: no panic, and they agree on what is a header.
    #[test]
    fn plan_header_on_garbage(bytes in lookalike(honest_header(), 200)) {
        let fixed = read_header_fixed(&bytes);
        let sw = read_header_sw(&bytes);
        if bytes.len() >= PLAN_HEADER_LEN {
            prop_assert_eq!(fixed, sw);
        }
        if let Ok(header) = fixed {
            // Whatever it holds, opening the descriptor does not panic.
            let d = descriptors();
            for trust in trusts() {
                let _ = open_descriptor(&header.descriptor, &trust, &d.expect);
            }
        }
    }

    /// A step: any bytes. The framing helper, the reader and the length agree, and none panics.
    #[test]
    fn step_on_garbage(bytes in lookalike(vec![3, 0, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8], 40)) {
        let framed = step_len_from_head(&bytes);
        let read = read_step(&bytes);
        if let Ok(view) = read {
            let len = step_len(&view);
            prop_assert!(len <= bytes.len());
            // A step read from a longer buffer is the same step when cut at its length.
            prop_assert_eq!(framed, Ok(Some(len)));
            prop_assert!(read_step(&bytes[..len]).is_ok());
            // And exactly one less byte is not a step.
            if len > 0 {
                prop_assert!(read_step(&bytes[..len - 1]).is_err());
            }
        }
        // Every prefix of the stream: Ok(None) until the head is there, then the length never changes.
        let mut seen = None;
        for end in 0..=bytes.len().min(32) {
            if let Ok(Some(len)) = step_len_from_head(&bytes[..end]) {
                prop_assert!(seen.is_none() || seen == Some(len));
                seen = Some(len);
            }
        }
    }

    /// The device manifest on the server: any bytes, and what parses encodes back to the same bytes.
    #[test]
    fn manifest_on_garbage(bytes in lookalike(honest_manifest(), 80)) {
        if let Ok(manifest) = Manifest::parse(&bytes) {
            prop_assert_eq!(manifest.encode(), bytes.clone());
        }
        let _ = ManifestHeader::from_fixed(&bytes);
        let _ = ChunkEntry::from_head_tail(&bytes);
    }

    /// A real manifest with bytes changed, deleted and inserted.
    #[test]
    fn manifest_mutated(edits in prop::collection::vec((0u8..3, any::<u32>(), any::<u8>()), 1..6)) {
        let mut bytes = honest_manifest();
        for (kind, at, value) in edits {
            if bytes.is_empty() {
                break;
            }
            let at = at as usize % bytes.len();
            match kind {
                0 => bytes[at] ^= value | 1,
                1 => {
                    bytes.remove(at);
                }
                _ => bytes.insert(at, value),
            }
        }
        if let Ok(manifest) = Manifest::parse(&bytes) {
            prop_assert_eq!(manifest.encode(), bytes.clone());
        }
    }
}

fn honest_header() -> Vec<u8> {
    let world = World::new::<G0>(2048, 4, signer());
    let plan = honest_plan(&world, &world.device::<G0>());
    plan[..PLAN_HEADER_LEN].to_vec()
}

fn honest_manifest() -> Vec<u8> {
    let world = World::new::<G0>(2048, 4, signer());
    world.device::<G0>().manifest().unwrap()
}

fn honest_plan<M: FlashModel, B: Backend<M>>(world: &World, device: &Device<M, B>) -> Vec<u8> {
    let manifest = device.manifest().unwrap();
    world.plan_for(&manifest, &world.new_release).encode()
}

// ---------------------------------------------------------------------------------------------------------
// The apply engine
// ---------------------------------------------------------------------------------------------------------

/// How a round of the fuzz gets from an honest plan to a hostile one. Raw numbers, read against the geometry.
#[derive(Clone, Debug)]
struct Round {
    /// Step-level edits: (kind, a, b, c).
    steps: Vec<(u8, u16, u16, u64)>,
    /// Byte-level edits of the body: (kind, position, value).
    body: Vec<(u8, u32, u8)>,
    /// Byte edits of the header: (position, xor).
    header: Option<Vec<(u8, u8)>>,
    /// Replace the body with junk of this length (seeded by the first body edit's value).
    junk_body: Option<u16>,
    /// Make the header's step count and body length match what is there.
    fixup: bool,
    /// Cut power during this round: after this many flash operations, torn this way.
    cut: Option<(u16, u64)>,
}

fn round() -> impl Strategy<Value = Round> {
    (
        prop::collection::vec((any::<u8>(), any::<u16>(), any::<u16>(), any::<u64>()), 0..4),
        prop::collection::vec((any::<u8>(), any::<u32>(), any::<u8>()), 0..4),
        prop::option::weighted(0.15, prop::collection::vec((0..PLAN_HEADER_LEN as u8, 1..=255u8), 1..3)),
        prop::option::weighted(0.1, 0..400u16),
        prop::bool::weighted(0.7),
        prop::option::weighted(0.6, (0..150u16, any::<u64>())),
    )
        .prop_map(|(steps, body, header, junk_body, fixup, cut)| Round {
            steps,
            body,
            header,
            junk_body,
            fixup,
            cut,
        })
}

fn mutate_steps(world: &World, steps: &mut Vec<PlanStep>, edits: &[(u8, u16, u16, u64)]) {
    let g = &world.geometry;
    let (chunk, chunks, blocks) = (g.chunk as usize, g.chunks() as usize, g.blocks() as usize);
    for &(kind, a, b, c) in edits {
        let len = steps.len();
        let at = a as usize % (len + 1);
        let existing = (len > 0).then(|| a as usize % len);
        match (kind % 12, existing) {
            (0, Some(i)) => {
                steps.remove(i);
            }
            (1, Some(i)) => steps.insert(i, steps[i].clone()),
            (2, Some(i)) => steps.swap(i, b as usize % len),
            (3, _) => steps.reverse(),
            (4, _) => steps.insert(
                at,
                PlanStep::Erase {
                    block: (b as usize % (blocks + 2)) as u16,
                },
            ),
            // A COPY with the right hash for a chunk of the running image: gets past the hash check.
            (5, _) => {
                let src = c as usize % chunks;
                let hash = ChunkHash::of(&world.old[src * chunk..(src + 1) * chunk]);
                steps.insert(
                    at,
                    PlanStep::Copy {
                        dst: (b as usize % (chunks + 2)) as u16,
                        src: src as u16,
                        hash,
                    },
                )
            }
            // A WRITE of junk with its right hash.
            (6, _) => {
                let data = bytes(c, chunk);
                steps.insert(
                    at,
                    PlanStep::Write {
                        dst: (b as usize % (chunks + 2)) as u16,
                        hash: ChunkHash::of(&data),
                        data,
                    },
                )
            }
            // A WRITE of the wrong size, hash right for what it holds.
            (7, _) => {
                let data = bytes(c, c as usize % (chunk + 3));
                steps.insert(
                    at,
                    PlanStep::Write {
                        dst: (b as usize % (chunks + 2)) as u16,
                        hash: ChunkHash::of(&data),
                        data,
                    },
                )
            }
            // Point a step somewhere else.
            (8, Some(i)) => match &mut steps[i] {
                PlanStep::Erase { block } => *block = b,
                PlanStep::Copy { dst, src, .. } => {
                    if c & 1 == 0 {
                        *dst = b
                    } else {
                        *src = b
                    }
                }
                PlanStep::Write { dst, .. } => *dst = b,
            },
            (9, Some(i)) => match &mut steps[i] {
                PlanStep::Erase { .. } => {}
                PlanStep::Copy { hash, .. } | PlanStep::Write { hash, .. } => hash.0[c as usize % 8] ^= 1 << (b % 8),
            },
            // Flip a bit of a WRITE's data, leaving the hash.
            (10, Some(i)) => {
                if let PlanStep::Write { data, .. } = &mut steps[i]
                    && !data.is_empty()
                {
                    let n = data.len();
                    data[c as usize % n] ^= 1 << (b % 8);
                }
            }
            (11, Some(i)) => steps.truncate(i),
            _ => {}
        }
    }
}

fn mutate_body(body: &mut Vec<u8>, edits: &[(u8, u32, u8)]) {
    for &(kind, at, value) in edits {
        if body.is_empty() {
            break;
        }
        let at = at as usize % body.len();
        match kind % 4 {
            0 => body[at] ^= value | 1,
            1 => body.truncate(at),
            2 => body.insert(at, value),
            _ => {
                body.remove(at);
            }
        }
    }
}

/// The plan of this round as bytes, and whether its descriptor is the honest one.
fn hostile_plan(world: &World, honest: &chunky_server::Plan, round: &Round) -> (Vec<u8>, bool) {
    let mut steps = honest.steps.clone();
    mutate_steps(world, &mut steps, &round.steps);
    let mut body: Vec<u8> = steps.iter().flat_map(PlanStep::encode).collect();
    let step_count = steps.len() as u32;
    if let Some(len) = round.junk_body {
        let seed = round.body.first().map_or(1, |e| e.2 as u64);
        body = bytes(seed, len as usize);
    }
    mutate_body(&mut body, &round.body);

    let mut header = [0; PLAN_HEADER_LEN];
    honest.header.to_fixed(&mut header).unwrap();
    let original = header;
    for &(at, xor) in round.header.iter().flatten() {
        header[at as usize] ^= xor;
    }
    if round.fixup {
        // Step count 9..13, body length 13..17.
        header[9..13].copy_from_slice(&step_count.to_le_bytes());
        header[13..17].copy_from_slice(&(body.len() as u32).to_le_bytes());
    }
    let descriptor_intact = header[17..] == original[17..];
    ([header.as_slice(), body.as_slice()].concat(), descriptor_intact)
}

fn flash_len(world: &World) -> usize {
    let g = &world.geometry;
    match g.backend {
        chunky::BackendKind::Bank => 2 * g.bank_len() as usize,
        _ => [g.state, g.floor, g.active, g.dfu]
            .iter()
            .map(|r| r.end() as usize)
            .max()
            .unwrap(),
    }
}

/// Everything outside DFU's image area and the state region, as the CPU sees it now.
fn outside(
    world: &World,
    flash: &chunky_sim::SimFlash<impl FlashModel>,
    state: chunky::Region,
    dfu: chunky::Region,
) -> Vec<u8> {
    // A bank at a time: an access does not cross from one into the other.
    let step = match world.geometry.backend {
        chunky::BackendKind::Bank => world.geometry.bank_len() as usize,
        _ => flash_len(world),
    };
    let mut mem: Vec<u8> = (0..flash_len(world) / step)
        .flat_map(|i| flash.dump(i * step, step))
        .collect();
    let image = world.geometry.active.len as usize;
    for region in [
        (dfu.offset as usize, image),
        (state.offset as usize, state.len as usize),
    ] {
        mem[region.0..region.0 + region.1].fill(0);
    }
    mem
}

/// Rounds of hostile plans with cuts inside them, on one device; then an honest update must settle.
fn fuzz_apply<M: FlashModel, B: Backend<M>>(
    chunk: u32,
    blocks: u32,
    rounds: &[Round],
    regions: impl Fn(&Device<M, B>) -> (chunky::Region, chunky::Region),
) -> Result<(), TestCaseError> {
    let world = World::on::<M, B>(chunk, blocks, signer());
    let device = world.device_on::<M, B>();
    // The old release has run once: the floor is its own.
    device.boot().expect("the first boot");
    for round in rounds {
        let honest_bytes = honest_plan(&world, &device);
        let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
        let honest = chunky_server::plan(
            &world.geometry,
            &manifest,
            &world.new_release,
            &world.store,
            &chunky_server::Policy::default(),
        )
        .unwrap();
        prop_assert_eq!(&honest.encode(), &honest_bytes);

        let (state, dfu) = regions(&device);
        let before = outside(&world, &device.flash.borrow(), state, dfu);
        let ops_before = device.flash.borrow().ops();
        let (plan, descriptor_intact) = hostile_plan(&world, &honest, round);
        {
            let mut flash = device.flash.borrow_mut();
            flash.disarm();
            if let Some((after, seed)) = round.cut {
                flash.cut_after(after as u64, seed);
            }
        }
        let result = device.apply(&plan);
        let dead = device.flash.borrow().is_dead();

        if !descriptor_intact && !dead {
            prop_assert!(result.is_err(), "a changed descriptor was accepted");
        }
        if !descriptor_intact {
            prop_assert_eq!(
                device.flash.borrow().ops(),
                ops_before,
                "flash touched before the descriptor passed"
            );
        }
        // Power comes back (a reset), then look at what is there.
        device.flash.borrow_mut().disarm();
        device.flash.borrow_mut().power_on();
        let after = outside(&world, &device.flash.borrow(), state, dfu);
        if before != after {
            let first = before.iter().zip(&after).position(|(a, b)| a != b);
            prop_assert!(
                false,
                "flash outside DFU's image area changed at {first:?} (geometry {:?}): {result:?}",
                world.geometry
            );
        }
        if result.is_ok() {
            prop_assert!(!dead || device.flash.borrow().ops() > ops_before);
        }
        if let Ok(BootState::Staged) = device.boot_state() {
            let staged = device.dump(chunky::Region {
                offset: dfu.offset,
                len: world.geometry.active.len,
            });
            prop_assert!(
                staged == world.new,
                "staged, but DFU does not hold the release: {result:?}"
            );
            // Nothing else may follow a staged image in this loop.
            break;
        }
        if let Err(ApplyError::Flash) = result {
            prop_assert!(dead, "the flash failed without a power cut");
        }
    }
    device.flash.borrow_mut().disarm();
    device.flash.borrow_mut().power_on();
    // Whatever the rounds left in DFU, the device boots a release and an honest update settles.
    run_update(&device, &world, App::Good, &[]);
    Ok(())
}

fn rounds() -> impl Strategy<Value = Vec<Round>> {
    prop::collection::vec(round(), 1..=4)
}

fn swap_regions<M: FlashModel>(device: &Device<M, Swap>) -> (chunky::Region, chunky::Region) {
    (device.geometry.state, device.geometry.dfu)
}

proptest! {
    #![proptest_config(cases(48))]

    #[test]
    fn apply_g0(rounds in rounds()) {
        fuzz_apply::<G0, Swap>(2048, 4, &rounds, swap_regions)?;
    }

    #[test]
    fn apply_f1_small_chunks(rounds in rounds()) {
        fuzz_apply::<F1, Swap>(512, 4, &rounds, swap_regions)?;
    }

    #[test]
    fn apply_h7_small_chunks_in_big_sectors(rounds in rounds()) {
        fuzz_apply::<H7Mini, Swap>(2048, 2, &rounds, swap_regions)?;
    }

    #[test]
    fn apply_rp2350(rounds in rounds()) {
        fuzz_apply::<Rp2350, Swap>(4096, 3, &rounds, swap_regions)?;
    }

    #[test]
    fn apply_on_the_bank_backend(rounds in rounds()) {
        fuzz_apply::<G0, Bank>(2048, 3, &rounds, |device| {
            let map = device.bank_map();
            (map.state, map.dfu)
        })?;
    }

    /// An unsigned device: the same plans, the other trust.
    #[test]
    fn apply_unsigned(rounds in rounds()) {
        let world = World::new::<F1>(1024, 4, Signer::Unsigned);
        let device = world.device::<F1>();
        device.boot().unwrap();
        for round in &rounds {
            let honest = {
                let manifest = Manifest::parse(&device.manifest().unwrap()).unwrap();
                chunky_server::plan(
                    &world.geometry,
                    &manifest,
                    &world.new_release,
                    &world.store,
                    &chunky_server::Policy::default(),
                )
                .unwrap()
            };
            let (plan, _) = hostile_plan(&world, &honest, round);
            if let Some((after, seed)) = round.cut {
                device.flash.borrow_mut().cut_after(after as u64, seed);
            }
            let _ = device.apply(&plan);
            device.flash.borrow_mut().disarm();
            device.flash.borrow_mut().power_on();
            if device.boot_state() == Ok(BootState::Staged) {
                break;
            }
        }
        run_update(&device, &world, App::Good, &[]);
    }
}
