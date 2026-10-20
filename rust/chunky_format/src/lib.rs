//! The chunky wire format as ShrinkWrap types (PROTO-3, a spike): one format, two readers.
//!
//! The image descriptor and the plan header are `#[derive_shrink_wrap(.., fixed_layout)]` types: their ShrinkWrap
//! encoding is a fixed little-endian byte layout, so the same bytes are read
//!
//! - by the generated ShrinkWrap path (`des_shrink_wrap` through a `BufReader`), what an app or a full
//!   bootloader with WireWeaver links anyway, and
//! - by the generated fixed-layout fast path (`from_fixed`, offset consts, SW-44): straight loads at constant
//!   offsets, no `BufReader`, for a minimal bootloader; and by the C reader generated from the same types
//!   (`c/chunky_format.h`).
//!
//! The steps are a `head_tail` enum (SW-46) with a typed `List<u8>` for a WRITE's data, read with `Step::view`
//! (SW-52: no copy of the data, only the lengths are checked), and the same bytes as the fixed-layout steps of
//! the first draft of format 2 (the vectors did not change).
//!
//! Golden vectors (`vectors/`, `c/golden.h`, made by `chunky_server::vectors`) are checked by all readers.
//!
//! Format 2, little-endian: what `chunky` reads and `chunky_server` writes (descriptor, plan, manifest), in octets
//! in SPEC.md.
//!
//! Descriptor, 128 bytes at the end of the image ([SignedDescriptor]): the 64-byte [DescriptorBody], then a
//! 64-byte seal over the domain string [DESCRIPTOR_DOMAIN] and the body's bytes.
//!
//! | Offset | Size | Field |
//! |---|---|---|
//! | 0 | 4 | magic `CKY2` |
//! | 4 | 1 | format, 2 |
//! | 5 | 1 | scheme: 1 Ed25519, 2 unsigned |
//! | 6 | 2 | reserved, zero |
//! | 8 | 4 | product id |
//! | 12 | 4 | version |
//! | 16 | 4 | security counter |
//! | 20 | 4 | image length |
//! | 24 | 4 | geometry id |
//! | 28 | 32 | SHA-256 of the image bytes `[0, image length)` |
//! | 60 | 4 | reserved, zero |
//! | 64 | 64 | seal: Ed25519 signature, or SHA-256 then 32 zero bytes |
//!
//! Plan header, 145 bytes ([PlanHeader]): magic `CKP2`, format 2, geometry id (u32), step count (u32), body length
//! (u32: bytes of steps and data after the header), the target's [SignedDescriptor].
//!
//! Steps ([Step]), a one-byte op, the fixed head of the variant, and for a WRITE its data as the tail:
//!
//! | Op | Bytes | Fields |
//! |---|---|---|
//! | 1 ERASE | 3 | erase block (u16) |
//! | 2 COPY | 13 | chunk of DFU (u16), chunk of ACTIVE (u16), chunk hash (8) |
//! | 3 WRITE | 13 + data length | chunk of DFU (u16), data length (u16, the list's count), chunk hash (8), then the data |
//!
//! Manifest ([ManifestHeader], 13 bytes, then [ChunkEntry] per chunk, ACTIVE then DFU): magic `CKM2`, format 2,
//! backend (u8), scheme (u8), geometry id (u32), chunks (u16); an entry is `0` and an 8-byte hash, `1` blank or
//! `2` unreadable.
#![no_std]

pub use shrink_wrap::head_tail::List;
use shrink_wrap::prelude::*;

/// Format of descriptor and plan.
pub const FORMAT: u8 = 2;
/// Descriptor magic.
pub const DESCRIPTOR_MAGIC: [u8; 4] = *b"CKY2";
/// Plan magic.
pub const PLAN_MAGIC: [u8; 4] = *b"CKP2";
/// Chunk hash: SHA-256 truncated to 8 bytes (MAN-2, ww_dfu `CHUNK_HASH_LEN`).
pub const CHUNK_HASH_LEN: usize = 8;
/// Length of a seal.
pub const SEAL_LEN: usize = 64;
/// Domain of the descriptor seal: a signature over a descriptor is never valid for anything else.
pub const DESCRIPTOR_DOMAIN: &[u8] = b"chunky image descriptor v2";

/// A malformed descriptor or plan, or one whose seal does not check.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormatError {
    /// Too short, an unknown op or scheme: what ShrinkWrap refuses.
    Wire(ShrinkWrapError),
    /// Not a descriptor or plan.
    BadMagic,
    /// A format this code does not know.
    BadFormat,
    /// Reserved bytes are not zero.
    Reserved,
    /// The seal does not check: not signed by the trusted key, or not sealed the way this device requires.
    Seal,
}

impl From<ShrinkWrapError> for FormatError {
    fn from(e: ShrinkWrapError) -> Self {
        FormatError::Wire(e)
    }
}

/// How a descriptor is sealed.
#[derive_shrink_wrap(borrowed, sized, fixed_layout, ww_repr = u8, derive(Clone, Copy, Debug, PartialEq, Eq))]
pub enum Scheme {
    /// Ed25519 signature of the release key.
    Ed25519 = 1,
    /// SHA-256 only: integrity, no authenticity.
    Unsigned = 2,
}

/// What the descriptor says about an image. Only ever trusted after its seal checked: see [SignedDescriptor::open].
#[derive_shrink_wrap(borrowed, fixed_layout, derive(Clone, Copy, Debug, PartialEq, Eq))]
pub struct DescriptorBody {
    /// [DESCRIPTOR_MAGIC].
    pub magic: [u8; 4],
    /// [FORMAT].
    pub format: u8,
    /// How the seal is made.
    pub scheme: Scheme,
    /// Zero.
    pub reserved: [u8; 2],
    /// Which product the image is for.
    pub product_id: u32,
    /// Release version.
    pub version: u32,
    /// Anti-rollback counter.
    pub security_counter: u32,
    /// Bytes the hash covers.
    pub image_len: u32,
    /// Geometry the image was built for.
    pub geometry_id: u32,
    /// SHA-256 of the image bytes before the descriptor.
    pub image_sha256: [u8; 32],
    /// Zero.
    pub reserved_end: [u8; 4],
}

impl DescriptorBody {
    /// Magic, format and reserved bytes.
    pub fn check(&self) -> Result<(), FormatError> {
        if self.magic != DESCRIPTOR_MAGIC {
            return Err(FormatError::BadMagic);
        }
        if self.format != FORMAT {
            return Err(FormatError::BadFormat);
        }
        if self.reserved != [0; 2] || self.reserved_end != [0; 4] {
            return Err(FormatError::Reserved);
        }
        Ok(())
    }
}

/// Checks a seal: `seal` over `domain` followed by `body`. The device's [Trust](chunky's `Trust`) implements it:
/// an Ed25519 verifier with the release key, or the SHA-256 check of an unsigned build.
pub trait SealCheck {
    /// True when `seal` is right for `domain || body`.
    fn check(&self, domain: &[u8], body: &[u8], seal: &[u8; SEAL_LEN]) -> bool;
}

/// A `Signed<T>` envelope: the encoded bytes of a fixed-layout `T` and a seal over them, as one fixed-layout type.
/// Its fields are only reachable through `open`, which checks the seal over the bytes and only then decodes
/// them: verify, then decode.
///
/// A macro and not a generic type: on stable Rust a generic struct cannot hold `[u8; T::ENCODED_LEN]`.
#[macro_export]
macro_rules! signed {
    ($(#[$meta:meta])* $name:ident, $body:ty, $domain:expr) => {
        $(#[$meta])*
        #[derive_shrink_wrap(borrowed, fixed_layout, derive(Clone, Copy, Debug, PartialEq, Eq))]
        pub struct $name {
            /// The body's encoded bytes, as sealed.
            pub body: [u8; <$body>::ENCODED_LEN],
            /// The seal over the domain and `body`.
            pub seal: [u8; $crate::SEAL_LEN],
        }

        impl $name {
            /// Domain of the seal.
            pub const DOMAIN: &'static [u8] = $domain;

            /// Checks the seal, then decodes and checks the body.
            pub fn open(&self, check: &impl $crate::SealCheck) -> Result<$body, $crate::FormatError> {
                if !check.check(Self::DOMAIN, &self.body, &self.seal) {
                    return Err($crate::FormatError::Seal);
                }
                let body = <$body>::from_fixed(&self.body)?;
                body.check()?;
                Ok(body)
            }

            /// Encodes `body` and seals it with `seal(domain, bytes)`.
            pub fn seal(body: &$body, seal: impl FnOnce(&[u8], &[u8]) -> [u8; $crate::SEAL_LEN]) -> Self {
                let mut bytes = [0; <$body>::ENCODED_LEN];
                body.to_fixed(&mut bytes).expect("the buffer is the body's length");
                let seal = seal(Self::DOMAIN, &bytes);
                Self { body: bytes, seal }
            }
        }
    };
}

signed!(
    /// The descriptor at the end of an image: [DescriptorBody] sealed with [DESCRIPTOR_DOMAIN].
    SignedDescriptor,
    DescriptorBody,
    DESCRIPTOR_DOMAIN
);

/// The plan header. Untrusted: everything in it is checked against the device or, for the descriptor, its seal.
#[derive_shrink_wrap(borrowed, fixed_layout, derive(Clone, Copy, Debug, PartialEq, Eq))]
pub struct PlanHeader {
    /// [PLAN_MAGIC].
    pub magic: [u8; 4],
    /// [FORMAT].
    pub format: u8,
    /// Geometry the plan was made for.
    pub geometry_id: u32,
    /// Steps that follow.
    pub step_count: u32,
    /// Bytes after the header: steps and WRITE data.
    pub body_len: u32,
    /// The image the plan produces.
    pub descriptor: SignedDescriptor,
}

impl PlanHeader {
    /// Magic and format.
    pub fn check(&self) -> Result<(), FormatError> {
        if self.magic != PLAN_MAGIC {
            return Err(FormatError::BadMagic);
        }
        if self.format != FORMAT {
            return Err(FormatError::BadFormat);
        }
        Ok(())
    }
}

/// One step of a plan, a head+tail enum (SW-46): the op byte, then the variant's fixed head, then for a WRITE its
/// data as the tail. The bytes are those of the fixed-layout steps of format 2: a list's `u16` count sits where
/// `data_len` is, and the tail is the data right after the 13-byte head.
#[derive_shrink_wrap(head_tail, ww_repr = u8, derive(Clone, Copy, Debug, PartialEq))]
pub enum Step<'i> {
    /// Erase one erase block of DFU.
    Erase {
        /// Erase block index.
        block: u16,
    } = 1,
    /// Copy one chunk of the running image into DFU.
    Copy {
        /// Chunk of DFU.
        dst: u16,
        /// Chunk of ACTIVE.
        src: u16,
        /// Hash the source must have.
        hash: [u8; CHUNK_HASH_LEN],
    } = 2,
    /// Program one chunk of DFU with `data`, which follows the step's head.
    Write {
        /// Chunk of DFU.
        dst: u16,
        /// The chunk's bytes (count in the head, bytes in the tail).
        data: List<'i, u8>,
        /// Hash the data must have.
        hash: [u8; CHUNK_HASH_LEN],
    } = 3,
}

/// Manifest magic.
pub const MANIFEST_MAGIC: [u8; 4] = *b"CKM2";

/// The header of a device manifest (MAN-2). Untrusted on the server: a hint for the planner, nothing more.
#[derive_shrink_wrap(borrowed, fixed_layout, derive(Clone, Copy, Debug, PartialEq, Eq))]
pub struct ManifestHeader {
    /// [MANIFEST_MAGIC].
    pub magic: [u8; 4],
    /// [FORMAT].
    pub format: u8,
    /// Boot backend of the device (`chunky::BackendKind`).
    pub backend: u8,
    /// Seal scheme the device accepts.
    pub scheme: Scheme,
    /// Geometry id of the device.
    pub geometry_id: u32,
    /// Chunks per area: entries for ACTIVE, then as many for DFU.
    pub chunks: u16,
}

impl ManifestHeader {
    /// Magic and format.
    pub fn check(&self) -> Result<(), FormatError> {
        if self.magic != MANIFEST_MAGIC {
            return Err(FormatError::BadMagic);
        }
        if self.format != FORMAT {
            return Err(FormatError::BadFormat);
        }
        Ok(())
    }
}

/// What the device found in one chunk: one entry per chunk, in chunk order, ACTIVE first, then DFU. A blank or
/// unreadable chunk has no hash bytes, so an entry with both cannot be written.
#[derive_shrink_wrap(head_tail, ww_repr = u8, derive(Clone, Copy, Debug, PartialEq, Eq))]
pub enum ChunkEntry {
    /// Readable data with this hash.
    Hash {
        /// SHA-256 of the chunk, truncated.
        hash: [u8; CHUNK_HASH_LEN],
    } = 0,
    /// Every byte reads `0xFF`.
    Blank = 1,
    /// Faults on ECC.
    Unreadable = 2,
}

/// Offset of a WRITE's data count in the step: after the op byte and `dst`.
pub const WRITE_COUNT_OFFSET: usize = 1 + 2;

/// Bytes of the step that starts with `head`, from its op byte and head alone, before the data of a WRITE is
/// there: what a receiver of a stream needs to frame a step. `Ok(None)` while `head` is shorter than the head of
/// its op (or empty); an unknown op is an error. A WRITE's count is read, not bounded: the device refuses a count
/// that is not one chunk (`chunky::Applier::step_len`) before it buffers the data.
pub fn step_len_from_head(head: &[u8]) -> Result<Option<usize>, FormatError> {
    let Some(&op) = head.first() else {
        return Ok(None);
    };
    let head_len = 1 + match op {
        Step::ERASE_TAG => Step::ERASE_HEAD_LEN,
        Step::COPY_TAG => Step::COPY_HEAD_LEN,
        Step::WRITE_TAG => Step::WRITE_HEAD_LEN,
        _ => return Err(FormatError::Wire(ShrinkWrapError::EnumFutureVersionOrMalformedData)),
    };
    if head.len() < head_len {
        return Ok(None);
    }
    if op != Step::WRITE_TAG {
        return Ok(Some(head_len));
    }
    let count = u16::from_le_bytes([head[WRITE_COUNT_OFFSET], head[WRITE_COUNT_OFFSET + 1]]);
    Ok(Some(head_len + count as usize))
}

/// Bytes of a step on the wire: its head, and for a WRITE its data.
pub fn step_len(step: &StepView<'_>) -> usize {
    match step {
        StepView::Erase(_) => 1 + Step::ERASE_HEAD_LEN,
        StepView::Copy(_) => 1 + Step::COPY_HEAD_LEN,
        StepView::Write(w) => 1 + Step::WRITE_HEAD_LEN + w.data().len(),
    }
}

/// Reads the step at the start of `buf`: `Step::view` checks the lengths and gives the step's fields at their
/// offsets, `data()` of a WRITE a `List<u8>` over the bytes in `buf` (no copy; `as_bytes()` is the slice).
pub fn read_step(buf: &[u8]) -> Result<StepView<'_>, FormatError> {
    Step::view(buf).map_err(FormatError::Wire)
}

/// Reads a plan header with the fast path and checks magic and format (not the seal: see [SignedDescriptor::open]).
pub fn read_header_fixed(buf: &[u8]) -> Result<PlanHeader, FormatError> {
    let header = PlanHeader::from_fixed(buf)?;
    header.check()?;
    Ok(header)
}

/// Reads a plan header through ShrinkWrap and checks magic and format.
pub fn read_header_sw(buf: &[u8]) -> Result<PlanHeader, FormatError> {
    let header = PlanHeader::from_ww_bytes(buf.get(..PlanHeader::ENCODED_LEN).unwrap_or(buf))?;
    header.check()?;
    Ok(header)
}

/// The C readers of these types, as `c/chunky_format.h` holds them.
pub fn write_c_header(out: &mut dyn core::fmt::Write) -> core::fmt::Result {
    use shrink_wrap::head_tail::HeadTail;
    shrink_wrap::head_tail::write_c_header(
        out,
        "CHUNKY_FORMAT_H",
        "chunky_",
        &[
            <Scheme as HeadTail>::write_c_ht,
            <DescriptorBody as HeadTail>::write_c_ht,
            <SignedDescriptor as HeadTail>::write_c_ht,
            <PlanHeader as HeadTail>::write_c_ht,
            <Step as HeadTail>::write_c_ht,
            <ManifestHeader as HeadTail>::write_c_ht,
            <ChunkEntry as HeadTail>::write_c_ht,
        ],
    )
}
