//! Golden vectors and the pinned layout of format 2 (PROTO-3): the shrink_wrap path and the fixed-layout fast
//! path write the same bytes, read them to the same values, and those bytes are the ones in `vectors/`. The C
//! reader (`c/chunky_format.h`, generated) checks the same vectors in `c/test.c` (`just test-c`).
//!
//! `CHUNKY_UPDATE_GOLDEN=1 cargo test -p chunky_format` rewrites `vectors/` and the files in `c/` after a deliberate
//! format change.

use std::fmt::Write as _;
use std::path::PathBuf;

use chunky_format::*;
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};
use shrink_wrap::prelude::*;

fn dir(sub: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(sub)
}

/// The file in the repository equals `content`, or is rewritten with `CHUNKY_UPDATE_GOLDEN=1`.
fn golden(path: PathBuf, content: &[u8]) {
    if std::env::var_os("CHUNKY_UPDATE_GOLDEN").is_some() {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(&path, content).unwrap();
        return;
    }
    let on_disk = std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    assert!(
        on_disk == content,
        "{} is not what the types produce; after a deliberate change run CHUNKY_UPDATE_GOLDEN=1 cargo test -p chunky_format",
        path.display()
    );
}

/// Throwaway release key from a fixed seed (never a real key).
fn key() -> SigningKey {
    SigningKey::from_bytes(&[7; 32])
}

struct Ed25519(VerifyingKey);

impl SealCheck for Ed25519 {
    fn check(&self, domain: &[u8], body: &[u8], seal: &[u8; SEAL_LEN]) -> bool {
        let message = [domain, body].concat();
        self.0
            .verify(&message, &ed25519_dalek::Signature::from_bytes(seal))
            .is_ok()
    }
}

fn hash8(data: &[u8]) -> [u8; CHUNK_HASH_LEN] {
    Sha256::digest(data)[..CHUNK_HASH_LEN].try_into().unwrap()
}

fn body() -> DescriptorBody {
    DescriptorBody {
        magic: DESCRIPTOR_MAGIC,
        format: FORMAT,
        scheme: Scheme::Ed25519,
        reserved: [0; 2],
        product_id: 0x0000_c0de,
        version: 0x0002_0001,
        security_counter: 3,
        image_len: 0x0001_ff80,
        geometry_id: 0x6a09_e667,
        image_sha256: Sha256::digest(b"chunky golden image").into(),
        reserved_end: [0; 4],
    }
}

fn descriptor() -> SignedDescriptor {
    SignedDescriptor::seal(&body(), |domain, bytes| {
        key().sign(&[domain, bytes].concat()).to_bytes()
    })
}

const DATA: [u8; 16] = [
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
];

fn steps() -> Vec<(Step, &'static [u8])> {
    vec![
        (Step::Erase { block: 3 }, &[]),
        (
            Step::Copy {
                dst: 2,
                src: 5,
                hash: hash8(b"chunk five"),
            },
            &[],
        ),
        (
            Step::Write {
                dst: 6,
                data_len: DATA.len() as u16,
                hash: hash8(&DATA),
            },
            &DATA,
        ),
        (Step::Erase { block: 0x0102 }, &[]),
    ]
}

/// The plan: header, then each step and its data. Written with the fast path, checked against shrink_wrap.
fn plan() -> Vec<u8> {
    let mut body = Vec::new();
    for (step, data) in steps() {
        let mut fixed = [0; Step::ENCODED_LEN];
        let n = step.to_fixed(&mut fixed).unwrap();
        let mut sw = [0; 64];
        assert_eq!(step.to_ww_bytes(&mut sw).unwrap(), &fixed[..n]);
        body.extend_from_slice(&fixed[..n]);
        body.extend_from_slice(data);
    }
    let header = PlanHeader {
        magic: PLAN_MAGIC,
        format: FORMAT,
        geometry_id: 0x6a09_e667,
        step_count: steps().len() as u32,
        body_len: body.len() as u32,
        descriptor: descriptor(),
    };
    let mut fixed = [0; PlanHeader::ENCODED_LEN];
    header.to_fixed(&mut fixed).unwrap();
    let mut sw = [0; 256];
    assert_eq!(header.to_ww_bytes(&mut sw).unwrap(), &fixed[..]);
    [&fixed[..], &body].concat()
}

#[test]
fn layout_is_pinned() {
    // the descriptor table of the crate docs
    assert_eq!(DescriptorBody::MAGIC_OFFSET, 0);
    assert_eq!(DescriptorBody::FORMAT_OFFSET, 4);
    assert_eq!(DescriptorBody::SCHEME_OFFSET, 5);
    assert_eq!(DescriptorBody::RESERVED_OFFSET, 6);
    assert_eq!(DescriptorBody::PRODUCT_ID_OFFSET, 8);
    assert_eq!(DescriptorBody::VERSION_OFFSET, 12);
    assert_eq!(DescriptorBody::SECURITY_COUNTER_OFFSET, 16);
    assert_eq!(DescriptorBody::IMAGE_LEN_OFFSET, 20);
    assert_eq!(DescriptorBody::GEOMETRY_ID_OFFSET, 24);
    assert_eq!(DescriptorBody::IMAGE_SHA256_OFFSET, 28);
    assert_eq!(DescriptorBody::RESERVED_END_OFFSET, 60);
    assert_eq!(DescriptorBody::ENCODED_LEN, 64);
    assert_eq!(SignedDescriptor::SEAL_OFFSET, 64);
    assert_eq!(SignedDescriptor::ENCODED_LEN, 128);
    // the plan header
    assert_eq!(PlanHeader::FORMAT_OFFSET, 4);
    assert_eq!(PlanHeader::GEOMETRY_ID_OFFSET, 5);
    assert_eq!(PlanHeader::STEP_COUNT_OFFSET, 9);
    assert_eq!(PlanHeader::BODY_LEN_OFFSET, 13);
    assert_eq!(PlanHeader::DESCRIPTOR_OFFSET, 17);
    assert_eq!(PlanHeader::ENCODED_LEN, 145);
    // the steps
    assert_eq!(Step::ERASE_LEN, 3);
    assert_eq!(Step::COPY_LEN, 13);
    assert_eq!(Step::COPY_HASH_OFFSET, 5);
    assert_eq!(Step::WRITE_LEN, 13);
    assert_eq!(Step::WRITE_DATA_LEN_OFFSET, 3);
    assert!(!Step::SAME_LEN);
    // and shrink_wrap agrees that they have no length of their own on the wire
    assert_eq!(
        <PlanHeader as SerializeShrinkWrap>::ELEMENT_SIZE,
        shrink_wrap::ElementSize::Sized { size_bits: 145 * 8 }
    );
}

#[test]
fn bytes_at_their_offsets() {
    let plan = plan();
    let h = &plan[..PlanHeader::ENCODED_LEN];
    assert_eq!(&h[..4], b"CKP2");
    assert_eq!(h[4], 2);
    assert_eq!(&h[5..9], &0x6a09_e667u32.to_le_bytes());
    assert_eq!(&h[9..13], &4u32.to_le_bytes());
    let d = &h[17..];
    assert_eq!(&d[..4], b"CKY2");
    assert_eq!(d[5], 1);
    assert_eq!(&d[8..12], &0x0000_c0deu32.to_le_bytes());
    assert_eq!(&d[28..60], Sha256::digest(b"chunky golden image").as_slice());
    let s = &plan[PlanHeader::ENCODED_LEN..];
    assert_eq!(&s[..3], &[1, 3, 0]);
    assert_eq!(&s[3..8], &[2, 2, 0, 5, 0]);
    assert_eq!(&s[16..21], &[3, 6, 0, 16, 0]);
    assert_eq!(&s[29..45], &DATA);
    assert_eq!(&s[45..], &[1, 2, 1]);
}

#[test]
fn both_readers_read_the_vectors() {
    let plan = plan();
    golden(dir("vectors/plan.bin"), &plan);
    golden(dir("vectors/descriptor.bin"), &descriptor().seal_bytes());

    let fixed = read_header_fixed(&plan).unwrap();
    let sw = read_header_sw(&plan).unwrap();
    assert_eq!(fixed, sw);
    let body = fixed.descriptor.open(&Ed25519(key().verifying_key())).unwrap();
    assert_eq!(body, self::body());

    let mut at = PlanHeader::ENCODED_LEN;
    for (step, data) in steps() {
        let f = read_step_fixed(&plan[at..]).unwrap();
        let s = read_step_sw(&plan[at..]).unwrap();
        assert_eq!(f, s);
        assert_eq!((f.step, f.data), (step, data));
        at += f.len;
    }
    assert_eq!(at, plan.len());
    assert_eq!(at - PlanHeader::ENCODED_LEN, fixed.body_len as usize);
}

trait SealBytes {
    fn seal_bytes(&self) -> Vec<u8>;
}

impl SealBytes for SignedDescriptor {
    fn seal_bytes(&self) -> Vec<u8> {
        let mut out = [0; SignedDescriptor::ENCODED_LEN];
        self.to_fixed(&mut out).unwrap();
        out.to_vec()
    }
}

#[test]
fn verify_then_decode() {
    let check = Ed25519(key().verifying_key());
    let good = descriptor();
    assert!(good.open(&check).is_ok());
    // any changed bit of the body or the seal
    for i in 0..SignedDescriptor::ENCODED_LEN {
        let mut bytes = good.seal_bytes();
        bytes[i] ^= 0x01;
        let bad = SignedDescriptor::from_fixed(&bytes).unwrap();
        assert_eq!(bad.open(&check), Err(FormatError::Seal), "byte {i}");
    }
    // another key
    let other = Ed25519(SigningKey::from_bytes(&[8; 32]).verifying_key());
    assert_eq!(good.open(&other), Err(FormatError::Seal));
    // a well-sealed body that is not a descriptor of this format: refused after the seal
    let mut wrong = body();
    wrong.format = 1;
    let sealed = SignedDescriptor::seal(&wrong, |domain, bytes| key().sign(&[domain, bytes].concat()).to_bytes());
    assert_eq!(sealed.open(&check), Err(FormatError::BadFormat));
    // an unknown scheme byte under a good seal: shrink_wrap refuses it
    let mut raw = [0; DescriptorBody::ENCODED_LEN];
    body().to_fixed(&mut raw).unwrap();
    raw[DescriptorBody::SCHEME_OFFSET] = 9;
    let sealed = SignedDescriptor {
        body: raw,
        seal: key().sign(&[DESCRIPTOR_DOMAIN, &raw[..]].concat()).to_bytes(),
    };
    assert!(matches!(sealed.open(&check), Err(FormatError::Wire(_))));
}

/// xorshift, deterministic.
struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

#[test]
fn garbage_never_panics_and_both_readers_agree() {
    let plan = plan();
    let mut rng = Rng(0x9e37_79b9_7f4a_7c15);
    for round in 0..20_000 {
        // random bytes, or the plan with a few bytes changed and cut short
        let buf: Vec<u8> = if round % 2 == 0 {
            let len = (rng.next() % 200) as usize;
            (0..len).map(|_| rng.next() as u8).collect()
        } else {
            let mut b = plan.clone();
            for _ in 0..3 {
                let i = (rng.next() as usize) % b.len();
                b[i] = rng.next() as u8;
            }
            b.truncate((rng.next() as usize) % (b.len() + 1));
            b
        };
        assert_eq!(read_header_fixed(&buf).ok(), read_header_sw(&buf).ok());
        let mut at = 0;
        loop {
            let f = read_step_fixed(&buf[at..]);
            let s = read_step_sw(&buf[at..]);
            assert_eq!(f.is_ok(), s.is_ok(), "{:02x?}", &buf[at..]);
            match (f, s) {
                (Ok(f), Ok(s)) => {
                    assert_eq!(f, s);
                    at += f.len;
                }
                _ => break,
            }
        }
    }
}

/// The generated C reader and the vectors as C arrays, both checked in.
#[test]
fn c_files() {
    let mut header = String::new();
    write_c_header(&mut header).unwrap();
    golden(dir("c/chunky_format.h"), header.as_bytes());

    let plan = plan();
    let body = body();
    let mut g = String::new();
    writeln!(
        g,
        "/* Golden vectors of format 2, generated by chunky_format/tests/golden.rs, do not edit. */"
    )
    .unwrap();
    writeln!(
        g,
        "#ifndef CHUNKY_GOLDEN_H\n#define CHUNKY_GOLDEN_H\n\n#include <stdint.h>\n"
    )
    .unwrap();
    writeln!(g, "static const uint8_t golden_plan[{}] = {{", plan.len()).unwrap();
    for line in plan.chunks(16) {
        let bytes: Vec<String> = line.iter().map(|b| format!("0x{b:02x}")).collect();
        writeln!(g, "    {},", bytes.join(", ")).unwrap();
    }
    writeln!(g, "}};\n").unwrap();
    for (name, value) in [
        ("GOLDEN_GEOMETRY_ID", 0x6a09_e667u32),
        ("GOLDEN_STEP_COUNT", steps().len() as u32),
        ("GOLDEN_PRODUCT_ID", body.product_id),
        ("GOLDEN_VERSION", body.version),
        ("GOLDEN_SECURITY_COUNTER", body.security_counter),
        ("GOLDEN_IMAGE_LEN", body.image_len),
        ("GOLDEN_COPY_SRC", 5),
        ("GOLDEN_WRITE_DST", 6),
        ("GOLDEN_WRITE_LEN", DATA.len() as u32),
        ("GOLDEN_LAST_ERASE", 0x0102),
    ] {
        writeln!(g, "#define {name} 0x{value:08x}u").unwrap();
    }
    writeln!(g, "\n#endif /* CHUNKY_GOLDEN_H */").unwrap();
    golden(dir("c/golden.h"), g.as_bytes());
}
