//! Building an image: payload, `0xFF` padding up to the partition, sealed descriptor.

use chunky::descriptor::{DESCRIPTOR_LEN, ImageDescriptor, PublicKey, Scheme, Trust, unsigned_seal};
use chunky::geometry::FlashGeometry;
use ed25519_dalek::{Signer as _, SigningKey};
use sha2::{Digest, Sha256};

/// What seals a descriptor.
pub enum Signer {
    /// The release key.
    Ed25519(Box<SigningKey>),
    /// No key: for devices built with the `unsigned` opt-out. Integrity only.
    Unsigned,
}

impl Signer {
    /// A release key from its 32-byte seed.
    pub fn from_seed(seed: &[u8; 32]) -> Self {
        Signer::Ed25519(Box::new(SigningKey::from_bytes(seed)))
    }

    /// What a device must trust to accept images from this signer.
    pub fn trust(&self) -> Trust {
        match self {
            Signer::Ed25519(key) => Trust::Ed25519(PublicKey(key.verifying_key().to_bytes())),
            Signer::Unsigned => Trust::Unsigned,
        }
    }

    fn scheme(&self) -> Scheme {
        self.trust().scheme()
    }
}

/// What a release says about itself.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageMeta {
    /// Product the image is for.
    pub product_id: u32,
    /// Release version.
    pub version: u32,
    /// Anti-rollback counter.
    pub security_counter: u32,
}

/// Why an image cannot be built.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ImageBuildError {
    /// The payload does not fit the partition.
    TooLarge {
        /// Bytes of payload.
        payload: usize,
        /// Bytes the partition has for it.
        room: usize,
    },
}

impl std::fmt::Display for ImageBuildError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ImageBuildError::TooLarge { payload, room } => {
                write!(f, "the payload is {payload} bytes, the partition has room for {room}")
            }
        }
    }
}

impl std::error::Error for ImageBuildError {}

/// The full image for ACTIVE: `payload`, padding, descriptor. Exactly `geometry.active.len` bytes.
pub fn build_image(
    payload: &[u8],
    geometry: &FlashGeometry,
    meta: &ImageMeta,
    signer: &Signer,
) -> Result<Vec<u8>, ImageBuildError> {
    let room = geometry.image_len() as usize;
    if payload.len() > room {
        return Err(ImageBuildError::TooLarge {
            payload: payload.len(),
            room,
        });
    }
    let mut image = vec![0xFF; room];
    image[..payload.len()].copy_from_slice(payload);

    let mut descriptor = ImageDescriptor {
        scheme: signer.scheme(),
        product_id: meta.product_id,
        version: meta.version,
        security_counter: meta.security_counter,
        image_len: room as u32,
        geometry_id: geometry.id(),
        image_sha256: Sha256::digest(&image).into(),
        signature: [0; 64],
    };
    let message = descriptor.signed_message();
    descriptor.signature = match signer {
        Signer::Ed25519(key) => key.sign(&message).to_bytes(),
        Signer::Unsigned => unsigned_seal(&message),
    };
    image.extend_from_slice(&descriptor.to_bytes());
    debug_assert_eq!(image.len(), room + DESCRIPTOR_LEN);
    Ok(image)
}
