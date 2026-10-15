//! `cargo run -p chunky_server --example plan_size -- OLD.bin NEW.bin`: plans the update from one firmware payload
//! to another with the planner for every preset geometry and prints what the plan holds and its size on the
//! wire (format 2): the numbers of FEATURES.md PROTO-3.

use chunky::chips::PRESETS;
use chunky::plan::PLAN_HEADER_LEN;
use chunky_server::image::{ImageMeta, Signer, build_image};
use chunky_server::manifest::Manifest;
use chunky_server::planner::{PlanStep, Policy, Release, plan};
use chunky_server::store::MemStore;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [old, new] = args.as_slice() else {
        eprintln!("usage: plan_size OLD.bin NEW.bin");
        std::process::exit(2);
    };
    let old = std::fs::read(old).expect("OLD");
    let new = std::fs::read(new).expect("NEW");
    let signer = Signer::from_seed(&[7; 32]);
    println!("| geometry | chunk | erase / copy / write | data | plan | overhead |");
    println!("|---|---:|---|---:|---:|---:|");
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
        let wire = p.encode().len();
        assert_eq!(wire, p.stats.wire_bytes);
        assert!(wire >= PLAN_HEADER_LEN);

        let (mut erase, mut copy, mut write, mut data) = (0, 0, 0, 0);
        for step in &p.steps {
            match step {
                PlanStep::Erase { .. } => erase += 1,
                PlanStep::Copy { .. } => copy += 1,
                PlanStep::Write { data: d, .. } => {
                    write += 1;
                    data += d.len();
                }
            }
        }
        println!(
            "| {name} | {} | {erase} / {copy} / {write} | {data} | {wire} | {} |",
            geometry.chunk,
            wire - data
        );
    }
}
