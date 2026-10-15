//! The device manifest: one entry per chunk of ACTIVE and of the part of DFU an image is staged into.
//!
//! The manifest is computed fresh from the flash every session and is the only progress record: after a power
//! cut the device hashes again and the server plans only what is still wrong. There is no journal.
//!
//! Wire format 2 (MAN-2, `chunky_format::ManifestHeader` and `ChunkEntry`), little-endian: a 13-byte header
//! (`CKM2`, format 2, backend, scheme, geometry id, chunk count), then `chunks` entries for ACTIVE and `chunks`
//! entries for DFU, in chunk order. An entry is a tag and, for a hash, its 8 bytes: `0` + hash, `1` blank, `2`
//! unreadable. The order is implicit and a blank chunk has no hash bytes, so neither can be written wrong.

use chunky_format::{ChunkEntry, FORMAT, MANIFEST_MAGIC};
use embedded_storage::nor_flash::ReadNorFlash;

use crate::descriptor::{Scheme, Trust};
use crate::ecc::{EccError, ReadFault, try_read};
use crate::geometry::FlashGeometry;
use crate::hash::{ChunkHash, is_blank};

pub use chunky_format::ManifestHeader;

/// Size of the header.
pub const HEADER_LEN: usize = ManifestHeader::ENCODED_LEN;
/// Size of the longest entry.
pub const MAX_ENTRY_LEN: usize = 1 + ChunkEntry::HASH_HEAD_LEN;

/// What the device found in one chunk.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChunkState {
    /// Readable data with this hash.
    Hash(ChunkHash),
    /// Erased: every byte reads `0xFF`. May be programmed without an erase.
    Blank,
    /// Faults on ECC (a write or erase cut by power loss). Needs an erase.
    Unreadable,
}

/// A manifest the device could not make.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestError {
    /// The flash failed while hashing.
    Flash,
}

impl ChunkState {
    /// Hashes the chunk at `offset`; `buf` is one chunk long.
    pub fn read<F>(flash: &mut F, offset: u32, buf: &mut [u8]) -> Result<Self, F::Error>
    where
        F: ReadNorFlash,
        F::Error: EccError,
    {
        match try_read(flash, offset, buf) {
            Ok(()) if is_blank(buf) => Ok(ChunkState::Blank),
            Ok(()) => Ok(ChunkState::Hash(ChunkHash::of(buf))),
            Err(ReadFault::Ecc) => Ok(ChunkState::Unreadable),
            Err(ReadFault::Flash(e)) => Err(e),
        }
    }

    /// The state of chunk content the server holds.
    pub fn of(data: &[u8]) -> Self {
        if is_blank(data) {
            ChunkState::Blank
        } else {
            ChunkState::Hash(ChunkHash::of(data))
        }
    }

    /// The wire entry.
    pub fn entry(&self) -> ChunkEntry {
        match *self {
            ChunkState::Hash(hash) => ChunkEntry::Hash { hash: hash.0 },
            ChunkState::Blank => ChunkEntry::Blank,
            ChunkState::Unreadable => ChunkEntry::Unreadable,
        }
    }

    /// From a wire entry.
    pub fn from_entry(entry: ChunkEntry) -> Self {
        match entry {
            ChunkEntry::Hash { hash } => ChunkState::Hash(ChunkHash(hash)),
            ChunkEntry::Blank => ChunkState::Blank,
            ChunkEntry::Unreadable => ChunkState::Unreadable,
        }
    }

    /// Wire form into `out` (at least [`MAX_ENTRY_LEN`] bytes); returns its length.
    pub fn encode(&self, out: &mut [u8; MAX_ENTRY_LEN]) -> usize {
        self.entry()
            .to_head_tail(out)
            .expect("the buffer holds the longest entry")
    }
}

/// The header a device with this geometry, accepting seals of `scheme`, sends.
pub fn manifest_header(geometry: &FlashGeometry, scheme: Scheme) -> ManifestHeader {
    ManifestHeader {
        magic: MANIFEST_MAGIC,
        format: FORMAT,
        backend: geometry.backend as u8,
        scheme,
        geometry_id: geometry.id(),
        chunks: geometry.chunks() as u16,
    }
}

/// Hashes ACTIVE and DFU and hands the manifest to `sink` piece by piece. `buf` is one chunk long.
pub fn write_manifest<A, D>(
    geometry: &FlashGeometry,
    trust: &Trust,
    active: &mut A,
    dfu: &mut D,
    buf: &mut [u8],
    mut sink: impl FnMut(&[u8]),
) -> Result<(), ManifestError>
where
    A: ReadNorFlash,
    A::Error: EccError,
    D: ReadNorFlash,
    D::Error: EccError,
{
    let chunk = geometry.chunk as usize;
    assert!(buf.len() >= chunk);
    let buf = &mut buf[..chunk];
    let mut header = [0; HEADER_LEN];
    manifest_header(geometry, trust.scheme())
        .to_fixed(&mut header)
        .expect("the buffer is the header's length");
    sink(&header);
    let mut entry = [0; MAX_ENTRY_LEN];
    for index in 0..geometry.chunks() {
        let state = ChunkState::read(active, index * geometry.chunk, buf).map_err(|_| ManifestError::Flash)?;
        let len = state.encode(&mut entry);
        sink(&entry[..len]);
    }
    for index in 0..geometry.chunks() {
        let state = ChunkState::read(dfu, index * geometry.chunk, buf).map_err(|_| ManifestError::Flash)?;
        let len = state.encode(&mut entry);
        sink(&entry[..len]);
    }
    Ok(())
}
