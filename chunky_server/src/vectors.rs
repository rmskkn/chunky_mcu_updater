//! The golden vectors of format 2 (PROTO-3), made from a fixed firmware pair and a fixed-seed key: the one
//! generator behind `chunky_format/vectors/` and the C reader's `chunky_format/c/golden.h`.
//!
//! A device on STM32G0 flash (2 KB erase, 256-byte chunks, two blocks of image) runs the old firmware; its DFU
//! holds leftovers in block 0 and is blank in block 1. The plan to the new firmware has ERASE, COPY and WRITE
//! steps. It is made twice, for a release key (`signed/`) and for the unsigned opt-out (`unsigned/`).
//!
//! `chunky_server/tests/vectors.rs` fails when the files differ from [`files`]; `just vectors` rewrites them.
//! `chunky_sim/tests/vectors.rs` applies the checked-in plans to a simulated device, `chunky_format` and the C
//! reader read the same bytes.

use std::fmt::Write as _;

use chunky::descriptor::{DESCRIPTOR_LEN, Trust};
use chunky::geometry::{BackendKind, Family, FlashGeometry, Region};

use crate::image::{ImageMeta, Signer, build_image};
use crate::manifest::Manifest;
use crate::planner::{Plan, PlanStep, Policy, Release, plan};
use crate::store::MemStore;

/// Seed of the throwaway release key behind `signed/` (never a real key).
pub const KEY_SEED: [u8; 32] = [7; 32];
/// Product id of both firmwares.
pub const PRODUCT: u32 = 0x434B_5931;

/// The geometry of the vectors: STM32G0, 256-byte chunks, two erase blocks of image, swap backend.
pub fn geometry() -> FlashGeometry {
    let (erase, write, blocks) = (2048, 8, 2);
    let geometry = FlashGeometry {
        family: Family::Stm32G0,
        backend: BackendKind::Swap,
        chunk: 256,
        erase,
        write,
        state: Region { offset: 0, len: erase },
        floor: Region {
            offset: erase,
            len: erase,
        },
        active: Region {
            offset: 2 * erase,
            len: blocks * erase,
        },
        dfu: Region {
            offset: (2 + blocks) * erase,
            len: (blocks + 1) * erase,
        },
    };
    geometry.validate().expect("the vectors' geometry is valid");
    geometry
}

/// Deterministic junk (xorshift).
pub fn junk(seed: u64, len: usize) -> Vec<u8> {
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

/// Everything one kind of vectors holds.
pub struct Set {
    /// `signed` or `unsigned`.
    pub name: &'static str,
    /// What the device trusts.
    pub trust: Trust,
    /// The running image, as ACTIVE holds it.
    pub old: Vec<u8>,
    /// The target image.
    pub new: Vec<u8>,
    /// The image area of DFU before the update: leftovers in block 0, blank in block 1.
    pub dfu_before: Vec<u8>,
    /// What the device sends: its manifest, wire form.
    pub manifest: Vec<u8>,
    /// The parsed manifest.
    pub parsed: Manifest,
    /// The plan the server answers with.
    pub plan: Plan,
    /// The plan, wire form.
    pub plan_bytes: Vec<u8>,
    /// The sealed descriptor of the target image.
    pub descriptor: Vec<u8>,
}

/// Builds one set. The firmware pair: the new one changes chunk 0, moves chunk 1 to chunk 2 and appends a chunk.
pub fn set(name: &'static str, signer: &Signer) -> Set {
    let geometry = geometry();
    let chunk = geometry.chunk as usize;
    let used = 6;
    let old_payload = junk(1, used * chunk);
    let mut new_payload = old_payload.clone();
    new_payload[..chunk].copy_from_slice(&junk(2, chunk));
    new_payload[2 * chunk..3 * chunk].copy_from_slice(&old_payload[chunk..2 * chunk]);
    new_payload.extend_from_slice(&junk(3, chunk));
    let meta = |version, security_counter| ImageMeta {
        product_id: PRODUCT,
        version,
        security_counter,
    };
    let old = build_image(&old_payload, &geometry, &meta(1, 1), signer).expect("fits");
    let new = build_image(&new_payload, &geometry, &meta(2, 2), signer).expect("fits");

    let area = geometry.active.len as usize;
    let mut dfu_before = vec![0xFF; area];
    dfu_before[..geometry.erase as usize].copy_from_slice(&junk(9, geometry.erase as usize));

    let trust = signer.trust();
    let parsed = Manifest::of_contents(&geometry, &trust, &old, &dfu_before);
    let manifest = parsed.encode();
    let mut store = MemStore::default();
    Release::add(&old, &geometry, &mut store).expect("old release");
    let release = Release::add(&new, &geometry, &mut store).expect("new release");
    let plan = plan(&geometry, &parsed, &release, &store, &Policy::default()).expect("a plan");
    let plan_bytes = plan.encode();
    let descriptor = new[new.len() - DESCRIPTOR_LEN..].to_vec();
    Set {
        name,
        trust,
        old,
        new,
        dfu_before,
        manifest,
        parsed,
        plan,
        plan_bytes,
        descriptor,
    }
}

/// Both sets.
pub fn sets() -> [Set; 2] {
    [
        set("signed", &Signer::from_seed(&KEY_SEED)),
        set("unsigned", &Signer::Unsigned),
    ]
}

/// Every generated file: path relative to `chunky_format/`, and content.
pub fn files() -> Vec<(String, Vec<u8>)> {
    let sets = sets();
    let mut out = Vec::new();
    for s in &sets {
        for (file, bytes) in [
            ("old.bin", &s.old),
            ("new.bin", &s.new),
            ("dfu_before.bin", &s.dfu_before),
            ("manifest.bin", &s.manifest),
            ("plan.bin", &s.plan_bytes),
            ("descriptor.bin", &s.descriptor),
        ] {
            out.push((format!("vectors/{}/{file}", s.name), bytes.clone()));
        }
    }
    out.push(("c/golden.h".into(), c_header(&sets).into_bytes()));
    out
}

fn c_array(out: &mut String, name: &str, bytes: &[u8]) {
    writeln!(out, "static const uint8_t {name}[{}] = {{", bytes.len()).unwrap();
    for line in bytes.chunks(16) {
        let line: Vec<String> = line.iter().map(|b| format!("0x{b:02x}")).collect();
        writeln!(out, "    {},", line.join(", ")).unwrap();
    }
    writeln!(out, "}};\n").unwrap();
}

/// `c/golden.h`: the plans and manifests as arrays, and what a reader must find in them, taken from the planner's
/// steps and the parsed manifest, not from the bytes.
fn c_header(sets: &[Set; 2]) -> String {
    let geometry = geometry();
    let mut g = String::new();
    g.push_str(
        "/* Golden vectors of format 2, generated by chunky_server::vectors (`just vectors`), do not edit. */\n",
    );
    g.push_str("#ifndef CHUNKY_GOLDEN_H\n#define CHUNKY_GOLDEN_H\n\n#include <stdint.h>\n\n");
    g.push_str(
        "/* What a step is: tag as on the wire (1 = ERASE, 2 = COPY, 3 = WRITE), a = block or dst, b = src or\n\
         * chunk length; hash is the one the step carries, data_at the offset of a WRITE's chunk in golden_*_written. */\n\
         typedef struct {\n    uint8_t tag;\n    uint32_t a;\n    uint32_t b;\n    uint8_t hash[8];\n    uint32_t data_at;\n} golden_step_t;\n\n\
         /* One manifest entry: tag 0 = hash, 1 = blank, 2 = unreadable. */\n\
         typedef struct {\n    uint8_t tag;\n    uint8_t hash[8];\n} golden_entry_t;\n\n",
    );
    for s in sets {
        let n = s.name;
        c_array(&mut g, &format!("golden_{n}_plan"), &s.plan_bytes);
        c_array(&mut g, &format!("golden_{n}_manifest"), &s.manifest);
        let written: Vec<u8> = s
            .plan
            .steps
            .iter()
            .flat_map(|step| match step {
                PlanStep::Write { data, .. } => data.clone(),
                _ => Vec::new(),
            })
            .collect();
        c_array(&mut g, &format!("golden_{n}_written"), &written);
        let mut data_at = 0;
        writeln!(
            g,
            "static const golden_step_t golden_{n}_steps[{}] = {{",
            s.plan.steps.len()
        )
        .unwrap();
        for step in &s.plan.steps {
            let line = match step {
                PlanStep::Erase { block } => format!("{{1u, {block}u, 0u, {{0}}, 0u}}"),
                PlanStep::Copy { dst, src, hash } => format!("{{2u, {dst}u, {src}u, {}, 0u}}", c_hash(&hash.0)),
                PlanStep::Write { dst, hash, data } => {
                    data_at += data.len();
                    format!(
                        "{{3u, {dst}u, {}u, {}, {}u}}",
                        data.len(),
                        c_hash(&hash.0),
                        data_at - data.len()
                    )
                }
            };
            writeln!(g, "    {line},").unwrap();
        }
        writeln!(g, "}};\n").unwrap();
        let entries: Vec<_> = s.parsed.active.iter().chain(&s.parsed.dfu).collect();
        writeln!(
            g,
            "static const golden_entry_t golden_{n}_entries[{}] = {{",
            entries.len()
        )
        .unwrap();
        for e in entries {
            let line = match e {
                chunky::manifest::ChunkState::Hash(h) => format!("{{0u, {}}}", c_hash(&h.0)),
                chunky::manifest::ChunkState::Blank => "{1u, {0}}".to_string(),
                chunky::manifest::ChunkState::Unreadable => "{2u, {0}}".to_string(),
            };
            writeln!(g, "    {line},").unwrap();
        }
        writeln!(g, "}};\n").unwrap();
        let body = &s.new[s.new.len() - DESCRIPTOR_LEN..][..64];
        let info = chunky_format::DescriptorBody::from_fixed(body).expect("a descriptor body");
        let upper = n.to_uppercase();
        for (name, value) in [
            ("GEOMETRY_ID", geometry.id()),
            ("STEP_COUNT", s.plan.steps.len() as u32),
            ("ENTRY_COUNT", 2 * geometry.chunks()),
            ("CHUNKS", geometry.chunks()),
            ("PRODUCT_ID", info.product_id),
            ("VERSION", info.version),
            ("SECURITY_COUNTER", info.security_counter),
            ("IMAGE_LEN", info.image_len),
        ] {
            writeln!(g, "#define GOLDEN_{upper}_{name} 0x{value:08x}u").unwrap();
        }
        writeln!(g).unwrap();
    }
    g.push_str("#endif /* CHUNKY_GOLDEN_H */\n");
    g
}

fn c_hash(hash: &[u8; 8]) -> String {
    let bytes: Vec<String> = hash.iter().map(|b| format!("0x{b:02x}")).collect();
    format!("{{{}}}", bytes.join(", "))
}
