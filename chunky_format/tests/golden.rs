//! Golden vectors and the pinned layout of format 2 (PROTO-3): the shrink_wrap path and the fixed-layout fast
//! path write the same bytes, read them to the same values, and those bytes are the ones in `vectors/`. The C
//! reader (`c/chunky_format.h`, generated) checks the same vectors in `c/test.c` (`just test-c`).
//!
//! The vectors themselves (`vectors/`, `c/golden.h`) are made by `chunky_server::vectors` from a firmware pair
//! (`just vectors`); this file reads them with both readers. Only the generated C reader `c/chunky_format.h` is
//! written from here: `CHUNKY_UPDATE_GOLDEN=1 cargo test -p chunky_format` rewrites it after a deliberate change.

use std::path::PathBuf;

use chunky_format::*;
use ed25519_dalek::{Signer, SigningKey, Verifier, VerifyingKey};
use sha2::{Digest, Sha256};
use shrink_wrap::head_tail::List;
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

/// The unsigned opt-out: the seal is SHA-256 over `domain || body`, then zeros.
struct UnsignedSeal;

impl SealCheck for UnsignedSeal {
    fn check(&self, domain: &[u8], body: &[u8], seal: &[u8; SEAL_LEN]) -> bool {
        let mut want = [0; SEAL_LEN];
        want[..32].copy_from_slice(&Sha256::digest([domain, body].concat()));
        *seal == want
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

fn steps() -> Vec<Step<'static>> {
    vec![
        Step::Erase { block: 3 },
        Step::Copy {
            dst: 2,
            src: 5,
            hash: hash8(b"chunk five"),
        },
        Step::Write {
            dst: 6,
            data: List::new(&DATA),
            hash: hash8(&DATA),
        },
        Step::Erase { block: 0x0102 },
    ]
}

/// The plan: header, then each step and its data. Written with the fast path, checked against shrink_wrap.
fn plan() -> Vec<u8> {
    let mut body = Vec::new();
    for step in steps() {
        let mut buf = [0; 64];
        let n = step.to_head_tail(&mut buf).unwrap();
        assert_eq!(n, step.head_tail_len());
        body.extend_from_slice(&buf[..n]);
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
    assert_eq!(Step::HEAD_LEN, 1);
    assert_eq!(Step::ERASE_HEAD_LEN, 2);
    assert_eq!(Step::COPY_HEAD_LEN, 12);
    assert_eq!(Step::COPY_HASH_OFFSET, 4);
    assert_eq!(Step::WRITE_HEAD_LEN, 12);
    assert_eq!(Step::WRITE_DATA_OFFSET, 2);
    assert_eq!(Step::WRITE_HASH_OFFSET, 4);
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

/// The vectors of `chunky_server::vectors`, read with both readers: the plan header, the descriptor opened
/// with the key (or the unsigned seal), every step as view and as value, the manifest.
#[test]
fn both_readers_read_the_vectors() {
    for kind in ["signed", "unsigned"] {
        let read = |file: &str| std::fs::read(dir(&format!("vectors/{kind}/{file}"))).unwrap();
        let plan = read("plan.bin");
        let fixed = read_header_fixed(&plan).unwrap();
        let sw = read_header_sw(&plan).unwrap();
        assert_eq!(fixed, sw);
        assert_eq!(fixed.descriptor.seal_bytes(), read("descriptor.bin"), "{kind}");
        let body = if kind == "signed" {
            fixed.descriptor.open(&Ed25519(key().verifying_key())).unwrap()
        } else {
            fixed.descriptor.open(&UnsignedSeal).unwrap()
        };
        assert_eq!(body.format, FORMAT);
        assert_eq!(
            body.scheme,
            if kind == "signed" {
                Scheme::Ed25519
            } else {
                Scheme::Unsigned
            }
        );
        // the descriptor is the one at the end of the target image, and describes it
        let new = read("new.bin");
        assert_eq!(
            &new[new.len() - SignedDescriptor::ENCODED_LEN..],
            &fixed.descriptor.seal_bytes()[..]
        );
        assert_eq!(body.image_len as usize, new.len() - SignedDescriptor::ENCODED_LEN);
        assert_eq!(
            body.image_sha256,
            <[u8; 32]>::from(Sha256::digest(&new[..body.image_len as usize]))
        );

        let (mut at, mut kinds) = (PlanHeader::ENCODED_LEN, [0u32; 3]);
        for _ in 0..fixed.step_count {
            let view = read_step(&plan[at..]).unwrap();
            let len = step_len(&view);
            // the view and the value read the same step
            let value = Step::from_head_tail(&plan[at..at + len]).unwrap();
            match (&view, value) {
                (StepView::Erase(_), Step::Erase { .. }) => kinds[0] += 1,
                (StepView::Copy(_), Step::Copy { .. }) => kinds[1] += 1,
                (StepView::Write(w), Step::Write { dst, data, hash }) => {
                    kinds[2] += 1;
                    assert_eq!((w.dst(), w.data().as_bytes(), w.hash()), (dst, data.as_bytes(), hash));
                    assert_eq!(hash, hash8(data.as_bytes()));
                    // the chunk is the new image's chunk dst
                    let c = data.as_bytes().len();
                    assert_eq!(data.as_bytes(), &new[dst as usize * c..][..c]);
                }
                _ => panic!("view and value disagree"),
            }
            at += len;
        }
        assert!(kinds.iter().all(|&n| n > 0), "{kind}: {kinds:?}");
        assert_eq!(at, plan.len());
        assert_eq!(at - PlanHeader::ENCODED_LEN, fixed.body_len as usize);

        let manifest = read("manifest.bin");
        let header = ManifestHeader::from_fixed(&manifest).unwrap();
        let mut at = ManifestHeader::ENCODED_LEN;
        for _ in 0..2 * header.chunks {
            at += ChunkEntry::from_head_tail(&manifest[at..]).unwrap().head_tail_len();
        }
        assert_eq!(at, manifest.len());
    }
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
fn garbage_never_panics_and_view_and_value_agree() {
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
            let rest = &buf[at..];
            let Ok(view) = read_step(rest) else {
                // what the view refuses the value reader refuses too
                assert!(Step::from_head_tail(rest).is_err(), "{rest:02x?}");
                break;
            };
            let len = step_len(&view);
            assert!(len <= rest.len());
            let value = Step::from_head_tail(&rest[..len]).unwrap();
            if let (StepView::Write(w), Step::Write { dst, data, hash }) = (&view, value) {
                assert_eq!((w.dst(), w.data().as_bytes(), w.hash()), (dst, data.as_bytes(), hash));
            }
            at += len;
        }
    }
}

/// The generated C reader, checked in.
#[test]
fn c_files() {
    let mut header = String::new();
    write_c_header(&mut header).unwrap();
    golden(dir("c/chunky_format.h"), header.as_bytes());
}

#[test]
fn manifest_layout_is_pinned() {
    assert_eq!(ManifestHeader::FORMAT_OFFSET, 4);
    assert_eq!(ManifestHeader::BACKEND_OFFSET, 5);
    assert_eq!(ManifestHeader::SCHEME_OFFSET, 6);
    assert_eq!(ManifestHeader::GEOMETRY_ID_OFFSET, 7);
    assert_eq!(ManifestHeader::CHUNKS_OFFSET, 11);
    assert_eq!(ManifestHeader::ENCODED_LEN, 13);
    let mut buf = [0; 16];
    let hash = ChunkEntry::Hash {
        hash: [1, 2, 3, 4, 5, 6, 7, 8],
    };
    assert_eq!(hash.to_head_tail(&mut buf).unwrap(), 9);
    assert_eq!(&buf[..9], &[0, 1, 2, 3, 4, 5, 6, 7, 8]);
    assert_eq!(ChunkEntry::Blank.to_head_tail(&mut buf).unwrap(), 1);
    assert_eq!(buf[0], 1);
    assert_eq!(ChunkEntry::Unreadable.to_head_tail(&mut buf).unwrap(), 1);
    assert_eq!(buf[0], 2);
    // a tag no entry has
    assert!(ChunkEntry::from_head_tail(&[3]).is_err());
}

/// The length a stream receiver reads from a step's head is the length the view gives the whole step, for the
/// vectors and for garbage alike.
#[test]
fn step_len_from_head_agrees_with_the_view() {
    let plan = plan();
    let mut at = PlanHeader::ENCODED_LEN;
    while at < plan.len() {
        let rest = &plan[at..];
        let len = step_len(&read_step(rest).unwrap());
        for cut in 0..rest.len().min(1 + Step::WRITE_HEAD_LEN) {
            let framed = step_len_from_head(&rest[..cut]).unwrap();
            assert!(framed.is_none() || framed == Some(len), "{cut}");
        }
        assert_eq!(
            step_len_from_head(&rest[..len.min(1 + Step::WRITE_HEAD_LEN)]).unwrap(),
            Some(len)
        );
        at += len;
    }
    let mut rng = Rng(0x2545_f491_4f6c_dd1d);
    for _ in 0..20_000 {
        let len = (rng.next() % 40) as usize;
        let buf: Vec<u8> = (0..len).map(|_| (rng.next() % 5) as u8).collect();
        match (step_len_from_head(&buf), read_step(&buf)) {
            (Ok(Some(n)), Ok(view)) => assert_eq!(n, step_len(&view)),
            // the head is there but not the data, or not even the head
            (Ok(Some(n)), Err(_)) => assert!(n > buf.len()),
            (Ok(None), view) => assert!(view.is_err()),
            (Err(_), view) => assert!(view.is_err()),
        }
    }
}
