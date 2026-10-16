//! The checked-in golden vectors (`chunky_format/vectors/`, made by `chunky_server::vectors`) applied end to
//! end: a simulated device in the vectors' state sends the manifest that is in them, accepts the plan that is in
//! them and ends up with the target image in DFU, then boots it.

use std::path::Path;

use chunky::Trust;
use chunky_server::Signer;
use chunky_server::vectors::{KEY_SEED, PRODUCT, geometry};
use chunky_sim::{Device, G0};

fn vector(kind: &str, file: &str) -> Vec<u8> {
    let path = Path::new(env!("CARGO_MANIFEST_DIR")).join(format!("../chunky_format/vectors/{kind}/{file}"));
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

fn device(kind: &str, trust: Trust) -> Device<G0> {
    let geometry = geometry();
    let device = Device::<G0>::new(geometry, trust, PRODUCT);
    device.install(&vector(kind, "old.bin"));
    device
        .flash
        .borrow_mut()
        .load(geometry.dfu.offset as usize, &vector(kind, "dfu_before.bin"));
    device
}

fn applies(kind: &str, trust: Trust) {
    let device = device(kind, trust);
    device.boot().expect("the old image boots");
    assert_eq!(
        device.manifest().unwrap(),
        vector(kind, "manifest.bin"),
        "{kind}: the device's manifest"
    );

    let image = device
        .apply(&vector(kind, "plan.bin"))
        .expect("the device accepts the plan");
    let new = vector(kind, "new.bin");
    assert_eq!(image.version, 2);
    assert!(
        device.dump(geometry().dfu)[..new.len()] == new[..],
        "{kind}: DFU is the target image"
    );

    // and the update goes through: staged, swapped, trial, confirmed
    device.mark_staged_unchecked().expect("staged");
    device.boot().unwrap();
    assert_eq!(device.dump(geometry().active), new, "{kind}: the target runs");
    device.confirm().unwrap();
    device.boot().unwrap();
    assert_eq!(device.floor(), 2);
}

#[test]
fn signed_vectors_apply() {
    applies("signed", Signer::from_seed(&KEY_SEED).trust());
}

#[test]
fn unsigned_vectors_apply() {
    applies("unsigned", Trust::Unsigned);
}

#[test]
fn signed_plan_is_refused_by_an_unsigned_device_and_back() {
    for (kind, trust) in [
        ("signed", Trust::Unsigned),
        ("unsigned", Signer::from_seed(&KEY_SEED).trust()),
    ] {
        let device = device(kind, trust);
        assert!(device.apply(&vector(kind, "plan.bin")).is_err(), "{kind}");
    }
}
