//! The per-chunk hash: SHA-256 truncated to 8 bytes (MAN-2).
//!
//! It only decides what to transfer. Whether the result is the released image is decided by the full SHA-256 in
//! the signed image descriptor, never by this.

use sha2::{Digest, Sha256};

/// Bytes of a chunk hash.
pub const HASH_LEN: usize = chunky_format::CHUNK_HASH_LEN;

/// Truncated SHA-256 of one chunk.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, PartialOrd, Ord)]
pub struct ChunkHash(pub [u8; HASH_LEN]);

impl ChunkHash {
    /// Hash of `data`.
    pub fn of(data: &[u8]) -> Self {
        let full = Sha256::digest(data);
        let mut out = [0; HASH_LEN];
        out.copy_from_slice(&full[..HASH_LEN]);
        Self(out)
    }
}

/// True when every byte is erased flash.
pub fn is_blank(data: &[u8]) -> bool {
    data.iter().all(|&b| b == 0xFF)
}
