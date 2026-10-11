//! The signed image descriptor: the last 128 bytes of the image.
//!
//! An image fills the whole ACTIVE partition: payload, `0xFF` padding, descriptor. The descriptor names the
//! product, the version, the security counter and the geometry, and carries the SHA-256 of everything before it.
//! An Ed25519 signature covers the descriptor, so nothing in ACTIVE is unsigned.
//!
//! Small-flash builds may opt out of the signature with the `unsigned` feature ([`Trust::Unsigned`]): the seal
//! is then a SHA-256 over the same bytes. That keeps integrity (bad luck, broken transfers) and the version
//! floor, and protects against nobody who can talk to the device. The scheme is a field of the sealed part,
//! and a device that trusts a key refuses every other scheme, so a signed device cannot be talked down.
//!
//! | Offset | Size | Field |
//! |---|---|---|
//! | 0 | 4 | magic `CKY1` |
//! | 4 | 1 | format, 1 |
//! | 5 | 1 | scheme: 1 Ed25519, 2 unsigned |
//! | 6 | 2 | reserved, zero |
//! | 8 | 4 | product id |
//! | 12 | 4 | version (informational, shown and compared by the server) |
//! | 16 | 4 | security counter (anti-rollback: never below the device's floor) |
//! | 20 | 4 | image length: partition size minus 128 |
//! | 24 | 4 | geometry id |
//! | 28 | 32 | SHA-256 of image bytes `[0, image length)` |
//! | 60 | 64 | seal over the domain string and bytes `[0, 60)`: the Ed25519 signature, or SHA-256 then 32 zero bytes |
//! | 124 | 4 | reserved, zero |
//!
//! Integers are little-endian.

use embedded_storage::nor_flash::ReadNorFlash;
use sha2::{Digest, Sha256};

use crate::ecc::{EccError, ReadFault, try_read};

/// Size of the descriptor.
pub const DESCRIPTOR_LEN: usize = 128;
/// Bytes of the descriptor the signature covers.
pub const SIGNED_LEN: usize = 60;
/// Descriptor magic.
pub const MAGIC: [u8; 4] = *b"CKY1";
/// Descriptor format this code reads.
pub const FORMAT: u8 = 1;
/// Domain separation: a signature over a descriptor is never valid for anything else.
pub const SIGN_DOMAIN: &[u8; 26] = b"chunky image descriptor v1";

/// Length of the signed message.
pub const SIGNED_MESSAGE_LEN: usize = SIGN_DOMAIN.len() + SIGNED_LEN;

/// An Ed25519 public key that release images are checked against.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PublicKey(pub [u8; 32]);

/// How a descriptor is sealed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Scheme {
    /// Ed25519 signature of the release key.
    Ed25519 = 1,
    /// SHA-256 only: integrity, no authenticity.
    Unsigned = 2,
}

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

/// The seal of an unsigned descriptor: SHA-256 over the signed message, then 32 zero bytes.
pub fn unsigned_seal(message: &[u8; SIGNED_MESSAGE_LEN]) -> [u8; 64] {
    let mut seal = [0; 64];
    seal[..32].copy_from_slice(&Sha256::digest(message));
    seal
}

/// The fields of a descriptor.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageDescriptor {
    /// How the descriptor is sealed.
    pub scheme: Scheme,
    /// Which product the image is for.
    pub product_id: u32,
    /// Release version.
    pub version: u32,
    /// Anti-rollback counter, raised only by releases that must not be rolled back past.
    pub security_counter: u32,
    /// Bytes the hash covers.
    pub image_len: u32,
    /// Geometry the image was built for.
    pub geometry_id: u32,
    /// SHA-256 of the image bytes before the descriptor.
    pub image_sha256: [u8; 32],
    /// The seal: an Ed25519 signature, or the SHA-256 of an unsigned descriptor.
    pub signature: [u8; 64],
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
    /// No descriptor.
    BadMagic,
    /// A descriptor format this code does not know.
    BadFormat,
    /// Reserved bytes are not zero.
    Reserved,
    /// Signed for another product.
    WrongProduct,
    /// Built for another flash layout.
    GeometryMismatch,
    /// The length does not match the partition.
    BadLength,
    /// Sealed with a scheme this device does not accept (an unsigned image on a signed device, or the reverse).
    WrongScheme,
    /// The signature (or the hash of an unsigned descriptor) does not verify.
    BadSignature,
    /// The security counter is below the device's floor.
    RolledBack,
    /// The image bytes do not match the signed hash.
    HashMismatch,
    /// Part of the image faults on ECC.
    Unreadable,
    /// The flash failed.
    Flash,
}

fn u32_at(raw: &[u8], at: usize) -> u32 {
    u32::from_le_bytes([raw[at], raw[at + 1], raw[at + 2], raw[at + 3]])
}

impl ImageDescriptor {
    /// Reads the fields. Checks the framing only; nothing is trusted before [`check`](Self::check).
    pub fn parse(raw: &[u8; DESCRIPTOR_LEN]) -> Result<Self, ImageError> {
        if raw[0..4] != MAGIC {
            return Err(ImageError::BadMagic);
        }
        if raw[4] != FORMAT {
            return Err(ImageError::BadFormat);
        }
        let scheme = match raw[5] {
            1 => Scheme::Ed25519,
            2 => Scheme::Unsigned,
            _ => return Err(ImageError::BadFormat),
        };
        if raw[6..8] != [0; 2] || raw[124..128] != [0; 4] {
            return Err(ImageError::Reserved);
        }
        let mut image_sha256 = [0; 32];
        image_sha256.copy_from_slice(&raw[28..60]);
        let mut signature = [0; 64];
        signature.copy_from_slice(&raw[60..124]);
        Ok(Self {
            scheme,
            product_id: u32_at(raw, 8),
            version: u32_at(raw, 12),
            security_counter: u32_at(raw, 16),
            image_len: u32_at(raw, 20),
            geometry_id: u32_at(raw, 24),
            image_sha256,
            signature,
        })
    }

    /// The descriptor as it lies in flash.
    pub fn to_bytes(&self) -> [u8; DESCRIPTOR_LEN] {
        let mut raw = [0; DESCRIPTOR_LEN];
        raw[0..4].copy_from_slice(&MAGIC);
        raw[4] = FORMAT;
        raw[5] = self.scheme as u8;
        raw[8..12].copy_from_slice(&self.product_id.to_le_bytes());
        raw[12..16].copy_from_slice(&self.version.to_le_bytes());
        raw[16..20].copy_from_slice(&self.security_counter.to_le_bytes());
        raw[20..24].copy_from_slice(&self.image_len.to_le_bytes());
        raw[24..28].copy_from_slice(&self.geometry_id.to_le_bytes());
        raw[28..60].copy_from_slice(&self.image_sha256);
        raw[60..124].copy_from_slice(&self.signature);
        raw
    }

    /// The message the signature is made over.
    pub fn signed_message(&self) -> [u8; SIGNED_MESSAGE_LEN] {
        let mut message = [0; SIGNED_MESSAGE_LEN];
        message[..SIGN_DOMAIN.len()].copy_from_slice(SIGN_DOMAIN);
        message[SIGN_DOMAIN.len()..].copy_from_slice(&self.to_bytes()[..SIGNED_LEN]);
        message
    }

    /// Checks everything that can be checked without the image bytes: product, geometry, length, the seal,
    /// then the floor.
    pub fn check(&self, trust: &Trust, expect: &Expect) -> Result<(), ImageError> {
        if self.product_id != expect.product_id {
            return Err(ImageError::WrongProduct);
        }
        if self.geometry_id != expect.geometry_id {
            return Err(ImageError::GeometryMismatch);
        }
        if self.image_len != expect.image_len {
            return Err(ImageError::BadLength);
        }
        if self.scheme != trust.scheme() {
            return Err(ImageError::WrongScheme);
        }
        match trust {
            #[cfg(feature = "ed25519")]
            Trust::Ed25519(key) => {
                use ed25519_dalek::{Signature, VerifyingKey};
                let key = VerifyingKey::from_bytes(&key.0).map_err(|_| ImageError::BadSignature)?;
                key.verify_strict(&self.signed_message(), &Signature::from_bytes(&self.signature))
                    .map_err(|_| ImageError::BadSignature)?;
            }
            #[cfg(feature = "unsigned")]
            Trust::Unsigned => {
                if self.signature != unsigned_seal(&self.signed_message()) {
                    return Err(ImageError::BadSignature);
                }
            }
        }
        if self.security_counter < expect.floor {
            return Err(ImageError::RolledBack);
        }
        Ok(())
    }
}

/// Verifies the image at the start of `flash`: descriptor, seal, floor, then the SHA-256 of the bytes.
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
    let descriptor = ImageDescriptor::parse(&raw)?;
    descriptor.check(trust, expect)?;

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
