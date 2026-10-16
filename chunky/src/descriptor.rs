//! The sealed image descriptor: the last 128 bytes of the image, format 2 (`chunky_format::SignedDescriptor`,
//! whose crate docs have the layout).
//!
//! An image fills the whole ACTIVE partition: payload, `0xFF` padding, descriptor. The descriptor is a 64-byte
//! body (product, version, security counter, image length, geometry, the SHA-256 of everything before the
//! descriptor) and a 64-byte seal over a domain string and exactly those 64 bytes. Nothing in ACTIVE is unsealed.
//!
//! Verify, then decode: [`open_descriptor`] checks the seal over the body's bytes before the body is decoded and a
//! field of it used, and there is no other way to get an [`ImageDescriptor`] out of untrusted bytes on a device.
//! Before the seal it refuses what is plainly not a descriptor for this device (magic, format, scheme, reserved
//! bytes, length, read from the raw bytes), so junk never costs a signature verification; those checks only refuse.
//!
//! Small-flash builds may opt out of the signature with the `unsigned` feature ([`Trust::Unsigned`]): the seal
//! is then a SHA-256 over the same bytes. That keeps integrity (bad luck, broken transfers) and the version
//! floor, and protects against nobody who can talk to the device. The scheme is a field of the sealed body,
//! and a device that trusts a key refuses every other seal, so a signed device cannot be talked down.

use chunky_format::{DESCRIPTOR_MAGIC, FORMAT, FormatError, SEAL_LEN, SealCheck, SignedDescriptor};
use embedded_storage::nor_flash::ReadNorFlash;
use sha2::{Digest, Sha256};

use crate::ecc::{EccError, ReadFault, try_read};

pub use chunky_format::{DESCRIPTOR_DOMAIN, DescriptorBody as ImageDescriptor, Scheme};

/// Size of the descriptor.
pub const DESCRIPTOR_LEN: usize = SignedDescriptor::ENCODED_LEN;

/// Longest domain and body a seal is checked over: the Ed25519 message is assembled on the stack.
#[cfg(feature = "ed25519")]
const MAX_SEALED_LEN: usize = 128;

/// An Ed25519 public key that release images are checked against.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PublicKey(pub [u8; 32]);

/// What the device trusts. Fixed at build time: a variant exists only when its feature is on, so a build
/// with `ed25519` alone has no code path that accepts an unsigned image, and a build with `unsigned` alone
/// carries no verifier.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Trust {
    /// Images signed with this release key, nothing else.
    #[cfg(feature = "ed25519")]
    Ed25519(PublicKey),
    /// Any image whose hashes are right. Protects against bad luck, not against people.
    #[cfg(feature = "unsigned")]
    Unsigned,
}

impl Trust {
    /// The scheme a descriptor must have for this device.
    pub fn scheme(&self) -> Scheme {
        match self {
            #[cfg(feature = "ed25519")]
            Trust::Ed25519(_) => Scheme::Ed25519,
            #[cfg(feature = "unsigned")]
            Trust::Unsigned => Scheme::Unsigned,
        }
    }
}

impl SealCheck for Trust {
    /// Ed25519: a strict verification of `seal` over `domain || body` with the release key. Unsigned: `seal` is
    /// [`unsigned_seal`] of the same bytes.
    fn check(&self, domain: &[u8], body: &[u8], seal: &[u8; SEAL_LEN]) -> bool {
        match self {
            #[cfg(feature = "ed25519")]
            Trust::Ed25519(key) => {
                use ed25519_dalek::{Signature, VerifyingKey};
                let mut message = [0; MAX_SEALED_LEN];
                let len = domain.len() + body.len();
                if len > MAX_SEALED_LEN {
                    return false;
                }
                message[..domain.len()].copy_from_slice(domain);
                message[domain.len()..len].copy_from_slice(body);
                let Ok(key) = VerifyingKey::from_bytes(&key.0) else {
                    return false;
                };
                key.verify_strict(&message[..len], &Signature::from_bytes(seal)).is_ok()
            }
            #[cfg(feature = "unsigned")]
            Trust::Unsigned => *seal == unsigned_seal(domain, body),
        }
    }
}

/// The seal of an unsigned descriptor: SHA-256 over `domain || body`, then 32 zero bytes.
pub fn unsigned_seal(domain: &[u8], body: &[u8]) -> [u8; SEAL_LEN] {
    let mut sha = Sha256::new();
    sha.update(domain);
    sha.update(body);
    let mut seal = [0; SEAL_LEN];
    seal[..32].copy_from_slice(&sha.finalize());
    seal
}

/// What the device requires of an image.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Expect {
    /// This device's product.
    pub product_id: u32,
    /// This device's geometry.
    pub geometry_id: u32,
    /// Image length of this geometry.
    pub image_len: u32,
    /// Lowest security counter the device accepts.
    pub floor: u32,
}

/// Why an image is refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageError {
    /// The sealed body is not a descriptor.
    BadMagic,
    /// The sealed body is a descriptor format (or scheme number) this code does not know.
    BadFormat,
    /// Reserved bytes of the sealed body are not zero.
    Reserved,
    /// Signed for another product.
    WrongProduct,
    /// Built for another flash layout.
    GeometryMismatch,
    /// The length does not match the partition.
    BadLength,
    /// The seal checks but the body names a scheme this device does not accept.
    WrongScheme,
    /// The seal does not check: not signed by the trusted key (or not the hash of an unsigned descriptor), and
    /// so also every descriptor sealed another way than this device's.
    BadSignature,
    /// The security counter is below the device's floor.
    RolledBack,
    /// The image bytes do not match the sealed hash.
    HashMismatch,
    /// Part of the image faults on ECC.
    Unreadable,
    /// The flash failed.
    Flash,
}

impl From<FormatError> for ImageError {
    fn from(e: FormatError) -> Self {
        match e {
            FormatError::Seal => ImageError::BadSignature,
            FormatError::BadMagic => ImageError::BadMagic,
            FormatError::Reserved => ImageError::Reserved,
            FormatError::BadFormat | FormatError::Wire(_) => ImageError::BadFormat,
        }
    }
}

/// Opens a descriptor. First the cheap checks on the raw bytes that need no key: magic, format, scheme, reserved
/// bytes and image length, read at their offsets. They can only refuse: a junk descriptor is turned away
/// without costing a signature verification. Then the seal over the body's exact bytes, and only then is the
/// body decoded and its fields used: product, geometry, floor (and again framing, scheme and length, now from
/// the sealed decode).
pub fn open_descriptor(
    signed: &SignedDescriptor,
    trust: &Trust,
    expect: &Expect,
) -> Result<ImageDescriptor, ImageError> {
    open_with(signed, trust, trust.scheme(), expect)
}

/// [`open_descriptor`] with any seal check, so tests can count the verifications.
fn open_with(
    signed: &SignedDescriptor,
    check: &impl SealCheck,
    scheme: Scheme,
    expect: &Expect,
) -> Result<ImageDescriptor, ImageError> {
    precheck(&signed.body, scheme, expect)?;
    let body = signed.open(check)?;
    if body.scheme != scheme {
        return Err(ImageError::WrongScheme);
    }
    if body.product_id != expect.product_id {
        return Err(ImageError::WrongProduct);
    }
    if body.geometry_id != expect.geometry_id {
        return Err(ImageError::GeometryMismatch);
    }
    if body.image_len != expect.image_len {
        return Err(ImageError::BadLength);
    }
    if body.security_counter < expect.floor {
        return Err(ImageError::RolledBack);
    }
    Ok(body)
}

/// The refusals that need no key, on the unsealed bytes. Never a reason to accept anything or to skip the seal.
fn precheck(raw: &[u8; ImageDescriptor::ENCODED_LEN], scheme: Scheme, expect: &Expect) -> Result<(), ImageError> {
    let at = |offset: usize, len: usize| &raw[offset..offset + len];
    if at(ImageDescriptor::MAGIC_OFFSET, 4) != DESCRIPTOR_MAGIC {
        return Err(ImageError::BadMagic);
    }
    if raw[ImageDescriptor::FORMAT_OFFSET] != FORMAT {
        return Err(ImageError::BadFormat);
    }
    match raw[ImageDescriptor::SCHEME_OFFSET] {
        s if s == scheme as u8 => {}
        s if s == Scheme::Ed25519 as u8 || s == Scheme::Unsigned as u8 => return Err(ImageError::WrongScheme),
        _ => return Err(ImageError::BadFormat),
    }
    if at(ImageDescriptor::RESERVED_OFFSET, 2) != [0; 2] || at(ImageDescriptor::RESERVED_END_OFFSET, 4) != [0; 4] {
        return Err(ImageError::Reserved);
    }
    if at(ImageDescriptor::IMAGE_LEN_OFFSET, 4) != expect.image_len.to_le_bytes() {
        return Err(ImageError::BadLength);
    }
    Ok(())
}

/// The descriptor's two parts as they lie in flash. Splitting the bytes reads nothing of them.
pub fn split_descriptor(raw: &[u8; DESCRIPTOR_LEN]) -> SignedDescriptor {
    let (body, seal) = raw.split_at(DESCRIPTOR_LEN - SEAL_LEN);
    SignedDescriptor {
        body: body.try_into().expect("the body is the descriptor less the seal"),
        seal: seal.try_into().expect("the seal is the end of the descriptor"),
    }
}

/// Verifies the image at the start of `flash`: descriptor (seal, then the fields, floor), then the SHA-256 of
/// the bytes.
///
/// `buf` is scratch of at least [`DESCRIPTOR_LEN`] bytes. An ECC fault anywhere makes the image
/// [`Unreadable`](ImageError::Unreadable), it never escapes as a fault.
pub fn verify_image<F>(
    flash: &mut F,
    trust: &Trust,
    expect: &Expect,
    buf: &mut [u8],
) -> Result<ImageDescriptor, ImageError>
where
    F: ReadNorFlash,
    F::Error: EccError,
{
    assert!(buf.len() >= DESCRIPTOR_LEN);
    let read = |flash: &mut F, offset: u32, buf: &mut [u8]| {
        try_read(flash, offset, buf).map_err(|e| match e {
            ReadFault::Ecc => ImageError::Unreadable,
            ReadFault::Flash(_) => ImageError::Flash,
        })
    };
    let mut raw = [0; DESCRIPTOR_LEN];
    read(flash, expect.image_len, &mut raw)?;
    let descriptor = open_descriptor(&split_descriptor(&raw), trust, expect)?;

    let mut sha = Sha256::new();
    let mut offset = 0;
    while offset < expect.image_len {
        let len = buf.len().min((expect.image_len - offset) as usize);
        read(flash, offset, &mut buf[..len])?;
        sha.update(&buf[..len]);
        offset += len as u32;
    }
    if sha.finalize().as_slice() != descriptor.image_sha256 {
        return Err(ImageError::HashMismatch);
    }
    Ok(descriptor)
}

#[cfg(test)]
mod tests {
    use core::cell::Cell;

    use super::*;

    /// A seal check that counts how often it is asked, and says yes to an unsigned seal.
    struct Counting(Cell<u32>);

    impl SealCheck for Counting {
        fn check(&self, domain: &[u8], body: &[u8], seal: &[u8; SEAL_LEN]) -> bool {
            self.0.set(self.0.get() + 1);
            *seal == unsigned_seal(domain, body)
        }
    }

    const EXPECT: Expect = Expect {
        product_id: 7,
        geometry_id: 0x1234,
        image_len: 0x1_ff80,
        floor: 1,
    };

    fn sealed(scheme: Scheme) -> SignedDescriptor {
        let body = ImageDescriptor {
            magic: DESCRIPTOR_MAGIC,
            format: FORMAT,
            scheme,
            reserved: [0; 2],
            product_id: EXPECT.product_id,
            version: 3,
            security_counter: 2,
            image_len: EXPECT.image_len,
            geometry_id: EXPECT.geometry_id,
            image_sha256: [9; 32],
            reserved_end: [0; 4],
        };
        SignedDescriptor::seal(&body, unsigned_seal)
    }

    #[test]
    fn junk_is_refused_without_a_seal_check() {
        let good = sealed(Scheme::Unsigned);
        let cases: [(&str, usize, u8, ImageError); 6] = [
            ("magic", ImageDescriptor::MAGIC_OFFSET, b'X', ImageError::BadMagic),
            ("format", ImageDescriptor::FORMAT_OFFSET, 1, ImageError::BadFormat),
            (
                "scheme of another trust",
                ImageDescriptor::SCHEME_OFFSET,
                Scheme::Ed25519 as u8,
                ImageError::WrongScheme,
            ),
            (
                "unknown scheme",
                ImageDescriptor::SCHEME_OFFSET,
                9,
                ImageError::BadFormat,
            ),
            (
                "reserved",
                ImageDescriptor::RESERVED_END_OFFSET + 3,
                1,
                ImageError::Reserved,
            ),
            ("length", ImageDescriptor::IMAGE_LEN_OFFSET, 0, ImageError::BadLength),
        ];
        for (name, offset, value, expected) in cases {
            let mut junk = good;
            junk.body[offset] = value;
            let check = Counting(Cell::new(0));
            assert_eq!(
                open_with(&junk, &check, Scheme::Unsigned, &EXPECT),
                Err(expected),
                "{name}"
            );
            assert_eq!(check.0.get(), 0, "{name}: the seal was checked");
        }
        // all-erased flash and all-zero bytes
        for fill in [0xFF, 0x00] {
            let junk = split_descriptor(&[fill; DESCRIPTOR_LEN]);
            let check = Counting(Cell::new(0));
            assert!(open_with(&junk, &check, Scheme::Unsigned, &EXPECT).is_err());
            assert_eq!(check.0.get(), 0);
        }
    }

    #[test]
    fn what_passes_the_cheap_checks_is_still_sealed() {
        let check = Counting(Cell::new(0));
        assert!(open_with(&sealed(Scheme::Unsigned), &check, Scheme::Unsigned, &EXPECT).is_ok());
        assert_eq!(check.0.get(), 1);

        // product and floor are fields of the body: only read after the seal
        let mut forged = sealed(Scheme::Unsigned);
        forged.body[ImageDescriptor::PRODUCT_ID_OFFSET] ^= 1;
        let check = Counting(Cell::new(0));
        assert_eq!(
            open_with(&forged, &check, Scheme::Unsigned, &EXPECT),
            Err(ImageError::BadSignature)
        );
        assert_eq!(check.0.get(), 1);
    }
}
