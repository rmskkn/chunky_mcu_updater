//! Reading a device manifest. Untrusted input: lengths and tags are checked, nothing is assumed.

use chunky::geometry::FlashGeometry;
use chunky::manifest::{ChunkState, HEADER_LEN, ManifestHeader, manifest_header};
use chunky::{Scheme, Trust};
use chunky_format::{ChunkEntry, FormatError};

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
    Malformed(FormatError),
}

impl Manifest {
    /// Parses the wire form: the header, then exactly `2 * chunks` entries and nothing after them.
    pub fn parse(bytes: &[u8]) -> Result<Self, ManifestParseError> {
        let header = bytes.first_chunk::<HEADER_LEN>().ok_or(ManifestParseError::BadLength)?;
        let header = ManifestHeader::from_fixed(header).map_err(|e| ManifestParseError::Malformed(e.into()))?;
        header.check().map_err(ManifestParseError::Malformed)?;
        let chunks = header.chunks as usize;
        let mut rest = &bytes[HEADER_LEN..];
        let mut entries = || -> Result<Vec<ChunkState>, ManifestParseError> {
            let mut states = Vec::with_capacity(chunks);
            for _ in 0..chunks {
                let entry = match ChunkEntry::from_head_tail(rest) {
                    Ok(entry) => entry,
                    Err(_) if rest.is_empty() => return Err(ManifestParseError::BadLength),
                    Err(e) => return Err(ManifestParseError::Malformed(e.into())),
                };
                rest = &rest[entry.head_tail_len()..];
                states.push(ChunkState::from_entry(entry));
            }
            Ok(states)
        };
        let active = entries()?;
        let dfu = entries()?;
        if !rest.is_empty() {
            return Err(ManifestParseError::BadLength);
        }
        Ok(Self { header, active, dfu })
    }

    /// Wire form, as the device sends it.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = vec![0; HEADER_LEN];
        self.header
            .to_fixed(&mut out)
            .expect("the buffer is the header's length");
        for state in self.active.iter().chain(&self.dfu) {
            let mut entry = [0; chunky::manifest::MAX_ENTRY_LEN];
            let len = state.encode(&mut entry);
            out.extend_from_slice(&entry[..len]);
        }
        out
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
            header: manifest_header(geometry, trust.scheme()),
            active: states(active),
            dfu: states(dfu),
        }
    }

    /// The manifest of a device running `active` (a full ACTIVE image) with an empty DFU, sealed as `scheme`.
    pub fn of_running_image(geometry: &FlashGeometry, scheme: Scheme, active: &[u8]) -> Self {
        let chunks = geometry.chunks() as usize;
        let mut states: Vec<_> = active
            .chunks(geometry.chunk as usize)
            .take(chunks)
            .map(ChunkState::of)
            .collect();
        states.resize(chunks, ChunkState::Blank);
        Self {
            header: manifest_header(geometry, scheme),
            active: states,
            dfu: vec![ChunkState::Blank; chunks],
        }
    }

    /// True when the device accepts images without a signature.
    pub fn unsigned(&self) -> bool {
        self.header.scheme == Scheme::Unsigned
    }
}
