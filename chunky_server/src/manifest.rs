//! Reading a device manifest. Untrusted input: lengths and tags are checked, nothing is assumed.

use chunky::geometry::FlashGeometry;
use chunky::manifest::{ChunkState, ENTRY_LEN, HEADER_LEN, ManifestError, ManifestHeader};
use chunky::{Scheme, Trust};

/// A parsed manifest.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest {
    /// The header.
    pub header: ManifestHeader,
    /// One entry per chunk of ACTIVE.
    pub active: Vec<ChunkState>,
    /// One entry per chunk of the image area of DFU.
    pub dfu: Vec<ChunkState>,
}

/// Why a manifest is refused.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ManifestParseError {
    /// Shorter or longer than its header says.
    BadLength,
    /// Header or entry is malformed.
    Malformed(ManifestError),
}

impl Manifest {
    /// Parses the wire form.
    pub fn parse(bytes: &[u8]) -> Result<Self, ManifestParseError> {
        let header = bytes.first_chunk::<HEADER_LEN>().ok_or(ManifestParseError::BadLength)?;
        let header = ManifestHeader::decode(header).map_err(ManifestParseError::Malformed)?;
        let chunks = header.chunks as usize;
        let body = &bytes[HEADER_LEN..];
        if body.len() != 2 * chunks * ENTRY_LEN {
            return Err(ManifestParseError::BadLength);
        }
        let mut entries = body.as_chunks::<ENTRY_LEN>().0.iter().map(ChunkState::decode);
        let active = entries.by_ref().take(chunks).collect::<Result<Vec<_>, _>>();
        let dfu = entries.collect::<Result<Vec<_>, _>>();
        Ok(Self {
            header,
            active: active.map_err(ManifestParseError::Malformed)?,
            dfu: dfu.map_err(ManifestParseError::Malformed)?,
        })
    }

    /// The manifest a device with these partition contents would send (for tools and tests).
    pub fn of_contents(geometry: &FlashGeometry, trust: &Trust, active: &[u8], dfu: &[u8]) -> Self {
        let states = |bytes: &[u8]| {
            let mut states: Vec<_> = bytes
                .chunks(geometry.chunk as usize)
                .take(geometry.chunks() as usize)
                .map(ChunkState::of)
                .collect();
            states.resize(geometry.chunks() as usize, ChunkState::Blank);
            states
        };
        Self {
            header: ManifestHeader {
                backend: geometry.backend as u8,
                scheme: trust.scheme() as u8,
                geometry_id: geometry.id(),
                chunks: geometry.chunks() as u16,
            },
            active: states(active),
            dfu: states(dfu),
        }
    }

    /// True when the device accepts images without a signature.
    pub fn unsigned(&self) -> bool {
        self.header.scheme == Scheme::Unsigned as u8
    }
}
