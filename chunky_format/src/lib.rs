//! The chunky wire format as shrink_wrap types (PROTO-3, a spike): one format, two readers.
//!
//! The image descriptor, the plan header and the plan steps are `#[derive_shrink_wrap(.., fixed_layout)]` types:
//! their shrink_wrap encoding is a fixed little-endian byte layout, so the same bytes are read
//!
//! - by the generated shrink_wrap path (`des_shrink_wrap` through a `BufReader`), what an app or a full
//!   bootloader with WireWeaver links anyway, and
//! - by the generated fixed-layout fast path (`from_fixed`, offset consts, SW-44): straight loads at constant
//!   offsets, no `BufReader`, for a minimal bootloader; and by the C reader generated from the same types
//!   (`c/chunky_format.h`).
//!
//! Golden vectors (`tests/golden.rs`, `c/golden.h`) are checked by all three.
//!
//! Format 2, little-endian. This is not yet what `chunky` reads (format 1, `chunky::plan`); see FEATURES.md
//! PROTO-3 for the numbers and what switching takes.
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
//! Steps ([Step]), a one-byte op and its fields; a WRITE is followed by its data:
//!
//! | Op | Bytes | Fields |
//! |---|---|---|
//! | 1 ERASE | 3 | erase block (u16) |
//! | 2 COPY | 13 | chunk of DFU (u16), chunk of ACTIVE (u16), chunk hash (8) |
//! | 3 WRITE | 13 + data length | chunk of DFU (u16), data length (u16), chunk hash (8), then the data |
#![no_std]

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
    /// Too short, an unknown op or scheme: what shrink_wrap refuses.
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

/// One step of a plan.
#[derive_shrink_wrap(
    borrowed,
    self_describing,
    fixed_layout,
    ww_repr = u8,
    derive(Clone, Copy, Debug, PartialEq, Eq)
)]
pub enum Step {
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
    /// Program one chunk of DFU with the `data_len` bytes that follow the step.
    Write {
        /// Chunk of DFU.
        dst: u16,
        /// Bytes of data after the step.
        data_len: u16,
        /// Hash the data must have.
        hash: [u8; CHUNK_HASH_LEN],
    } = 3,
}

impl Step {
    /// Bytes of data after the step.
    pub fn data_len(&self) -> usize {
        match self {
            Step::Write { data_len, .. } => *data_len as usize,
            _ => 0,
        }
    }
}

/// A step and its data, and how many bytes they took.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StepRead<'a> {
    /// The step.
    pub step: Step,
    /// Data of a WRITE, empty otherwise.
    pub data: &'a [u8],
    /// Bytes of step and data.
    pub len: usize,
}

/// Reads the step at the start of `buf` with the fixed-layout fast path.
pub fn read_step_fixed(buf: &[u8]) -> Result<StepRead<'_>, FormatError> {
    let step = Step::from_fixed(buf)?;
    let at = step.fixed_len();
    let len = at + step.data_len();
    let data = buf
        .get(at..len)
        .ok_or(FormatError::Wire(ShrinkWrapError::OutOfBoundsReadRawSlice))?;
    Ok(StepRead { step, data, len })
}

/// Reads the step at the start of `buf` through shrink_wrap's `BufReader`.
pub fn read_step_sw(buf: &[u8]) -> Result<StepRead<'_>, FormatError> {
    let mut rd = BufReader::new(buf);
    let step = Step::des_shrink_wrap(&mut rd)?;
    let data = rd.read_raw_slice(step.data_len())?;
    let len = buf.len() - rd.bytes_left();
    Ok(StepRead { step, data, len })
}

/// Reads a plan header with the fast path and checks magic and format (not the seal: see [SignedDescriptor::open]).
pub fn read_header_fixed(buf: &[u8]) -> Result<PlanHeader, FormatError> {
    let header = PlanHeader::from_fixed(buf)?;
    header.check()?;
    Ok(header)
}

/// Reads a plan header through shrink_wrap and checks magic and format.
pub fn read_header_sw(buf: &[u8]) -> Result<PlanHeader, FormatError> {
    let header = PlanHeader::from_ww_bytes(buf.get(..PlanHeader::ENCODED_LEN).unwrap_or(buf))?;
    header.check()?;
    Ok(header)
}

/// The C readers of these types, as `c/chunky_format.h` holds them.
pub fn write_c_header(out: &mut dyn core::fmt::Write) -> core::fmt::Result {
    shrink_wrap::fixed_layout::write_c_header(
        out,
        "CHUNKY_FORMAT_H",
        "chunky_",
        &[
            <Scheme as FixedLayout>::write_c,
            <DescriptorBody as FixedLayout>::write_c,
            <SignedDescriptor as FixedLayout>::write_c,
            <PlanHeader as FixedLayout>::write_c,
            <Step as FixedLayout>::write_c,
        ],
    )
}
