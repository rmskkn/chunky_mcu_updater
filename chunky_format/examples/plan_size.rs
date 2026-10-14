//! `cargo run -p chunky_format --example plan_size -- OLD.bin NEW.bin`: plans the update from one firmware payload
//! to another with chunky's planner for every preset geometry, and prints the plan's size in wire format 1
//! (`chunky::plan`) and re-encoded in format 2 (this crate). The numbers of FEATURES.md PROTO-3.

use chunky::chips::PRESETS;
use chunky::plan::Step as Step1;
use chunky_format::{CHUNK_HASH_LEN, PlanHeader, Step};
use chunky_server::image::{ImageMeta, Signer, build_image};
use chunky_server::manifest::Manifest;
use chunky_server::planner::{Policy, Release, plan};
use chunky_server::store::MemStore;

fn short(hash: &[u8]) -> [u8; CHUNK_HASH_LEN] {
    hash[..CHUNK_HASH_LEN].try_into().unwrap()
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [old, new] = args.as_slice() else {
        eprintln!("usage: plan_size OLD.bin NEW.bin");
        std::process::exit(2);
    };
    let old = std::fs::read(old).expect("OLD");
    let new = std::fs::read(new).expect("NEW");
    let signer = Signer::from_seed(&[7; 32]);
    println!("| geometry | chunk | erase / copy / write | data | format 1 | format 2 | overhead 1 | overhead 2 |");
    println!("|---|---:|---|---:|---:|---:|---:|---:|");
    for (name, geometry) in PRESETS {
        let meta = |version| ImageMeta {
            product_id: 1,
            version,
            security_counter: 0,
        };
        let (Ok(a), Ok(b)) = (
            build_image(&old, geometry, &meta(1), &signer),
            build_image(&new, geometry, &meta(2), &signer),
        ) else {
            continue;
        };
        let mut store = MemStore::default();
        let release = Release::add(&b, geometry, &mut store).unwrap();
        let manifest = Manifest::of_running_image(geometry, release.info().scheme, &a);
        let p = plan(geometry, &manifest, &release, &store, &Policy::default()).unwrap();
        let v1 = p.encode().len();

        let (mut erase, mut copy, mut write, mut data) = (0, 0, 0, 0);
        let mut v2 = PlanHeader::ENCODED_LEN;
        let mut buf = [0; Step::ENCODED_LEN];
        for (step, payload) in &p.steps {
            let step = match *step {
                Step1::Erase { block } => {
                    erase += 1;
                    Step::Erase { block }
                }
                Step1::Copy { dst, src, hash } => {
                    copy += 1;
                    Step::Copy {
                        dst,
                        src,
                        hash: short(&hash.0),
                    }
                }
                Step1::Write { dst, hash } => {
                    write += 1;
                    Step::Write {
                        dst,
                        data_len: payload.as_ref().unwrap().len() as u16,
                        hash: short(&hash.0),
                    }
                }
            };
            v2 += step.to_fixed(&mut buf).unwrap();
            let n = payload.as_ref().map_or(0, Vec::len);
            v2 += n;
            data += n;
        }
        println!(
            "| {name} | {} | {erase} / {copy} / {write} | {data} | {v1} | {v2} | {} | {} |",
            geometry.chunk,
            v1 - data,
            v2 - data
        );
    }
}
