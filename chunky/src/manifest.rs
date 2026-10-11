//! The device manifest: one entry per chunk of ACTIVE and of the part of DFU an image is staged into.
//!
//! The manifest is computed fresh from the flash every session and is the only progress record: after a power
//! cut the device hashes again and the server plans only what is still wrong. There is no journal.
//!
//! Wire format, little-endian: a 16-byte header (`CKMF`, format 1, backend, scheme, 1 reserved, geometry id, chunk
//! count, 2 reserved), then `chunks` entries for ACTIVE and `chunks` entries for DFU. An entry is 17 bytes: a tag
//! (0 hash, 1 blank, 2 unreadable) and 16 hash bytes (zero unless the tag is 0).

use embedded_storage::nor_flash::ReadNorFlash;

use crate::descriptor::Trust;
use crate::ecc::{EccError, ReadFault, try_read};
use crate::geometry::FlashGeometry;
use crate::hash::{ChunkHash, HASH_LEN, is_blank};

/// Manifest magic.
pub const MAGIC: [u8; 4] = *b"CKMF";
/// Manifest format.
pub const FORMAT: u8 = 1;
/// Size of the header.
pub const HEADER_LEN: usize = 16;
/// Size of one entry.
pub const ENTRY_LEN: usize = 1 + HASH_LEN;

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

/// A malformed manifest.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestError {
    /// Not a manifest.
    BadMagic,
    /// A format this code does not know.
    BadFormat,
    /// Reserved bytes are not zero, or an unknown tag.
    Malformed,
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

    /// Wire form.
    pub fn encode(&self) -> [u8; ENTRY_LEN] {
        let mut out = [0; ENTRY_LEN];
        match self {
            ChunkState::Hash(hash) => out[1..].copy_from_slice(&hash.0),
            ChunkState::Blank => out[0] = 1,
            ChunkState::Unreadable => out[0] = 2,
        }
        out
    }

    /// Parses the wire form.
    pub fn decode(raw: &[u8; ENTRY_LEN]) -> Result<Self, ManifestError> {
        let mut hash = [0; HASH_LEN];
        hash.copy_from_slice(&raw[1..]);
        match raw[0] {
            0 => Ok(ChunkState::Hash(ChunkHash(hash))),
            1 | 2 if hash != [0; HASH_LEN] => Err(ManifestError::Malformed),
            1 => Ok(ChunkState::Blank),
            2 => Ok(ChunkState::Unreadable),
            _ => Err(ManifestError::Malformed),
        }
    }
}

/// The manifest header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ManifestHeader {
    /// Backend kind of the device, as [`BackendKind`](crate::BackendKind) on the wire.
    pub backend: u8,
    /// Seal scheme the device accepts, as [`Scheme`](crate::Scheme) on the wire: the server warns about every
    /// unsigned device and never plans an image sealed another way.
    pub scheme: u8,
    /// Geometry id of the device.
    pub geometry_id: u32,
    /// Chunks per area.
    pub chunks: u16,
}

impl ManifestHeader {
    /// Wire form.
    pub fn encode(&self) -> [u8; HEADER_LEN] {
        let mut out = [0; HEADER_LEN];
        out[0..4].copy_from_slice(&MAGIC);
        out[4] = FORMAT;
        out[5] = self.backend;
        out[6] = self.scheme;
        out[8..12].copy_from_slice(&self.geometry_id.to_le_bytes());
        out[12..14].copy_from_slice(&self.chunks.to_le_bytes());
        out
    }

    /// Parses the wire form.
    pub fn decode(raw: &[u8; HEADER_LEN]) -> Result<Self, ManifestError> {
        if raw[0..4] != MAGIC {
            return Err(ManifestError::BadMagic);
        }
        if raw[4] != FORMAT {
            return Err(ManifestError::BadFormat);
        }
        if raw[7] != 0 || raw[14..16] != [0; 2] {
            return Err(ManifestError::Malformed);
        }
        Ok(Self {
            backend: raw[5],
            scheme: raw[6],
            geometry_id: u32::from_le_bytes([raw[8], raw[9], raw[10], raw[11]]),
            chunks: u16::from_le_bytes([raw[12], raw[13]]),
        })
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
    let header = ManifestHeader {
        backend: geometry.backend as u8,
        scheme: trust.scheme() as u8,
        geometry_id: geometry.id(),
        chunks: geometry.chunks() as u16,
    };
    sink(&header.encode());
    for index in 0..geometry.chunks() {
        let state = ChunkState::read(active, index * geometry.chunk, buf).map_err(|_| ManifestError::Flash)?;
        sink(&state.encode());
    }
    for index in 0..geometry.chunks() {
        let state = ChunkState::read(dfu, index * geometry.chunk, buf).map_err(|_| ManifestError::Flash)?;
        sink(&state.encode());
    }
    Ok(())
}
