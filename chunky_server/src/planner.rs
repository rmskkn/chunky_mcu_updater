//! The planner: a device manifest and a target release in, a plan out.
//!
//! Per erase block of the destination:
//!
//! 1. Every chunk already right: no step.
//! 2. Every wrong chunk is blank on the device, and blank chunks are trusted: program just those.
//! 3. Otherwise: erase the block and program every chunk of it that is not blank in the target.
//!
//! A chunk is programmed by COPY when the running image holds the same bytes anywhere (zero bytes on the wire),
//! else by WRITE. COPY sources are only ever in ACTIVE, which no step can write, so the order of steps cannot
//! invalidate a source.

use std::collections::HashMap;
use std::io;

use chunky::descriptor::{DESCRIPTOR_LEN, ImageDescriptor, split_descriptor};
use chunky::geometry::FlashGeometry;
use chunky::manifest::ChunkState;
use chunky::plan::{PLAN_HEADER_LEN, PlanHeader, Step};
use chunky::{ChunkHash, ImageError};
use chunky_format::{FORMAT, List, PLAN_MAGIC};

use crate::image::read_descriptor_unverified;

use crate::manifest::Manifest;
use crate::store::BlobStore;

/// A released image, as the planner needs it: its descriptor and the state of every chunk.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Release {
    /// The image's descriptor, as it lies at the end of the image.
    pub descriptor: [u8; DESCRIPTOR_LEN],
    /// Hash or blank, per chunk.
    pub chunks: Vec<ChunkState>,
}

impl Release {
    /// Registers `image` (a full ACTIVE partition): stores its chunks and returns the release.
    pub fn add(image: &[u8], geometry: &FlashGeometry, store: &mut impl BlobStore) -> Result<Self, PlanningError> {
        if image.len() != geometry.active.len as usize {
            return Err(PlanningError::BadImage(ImageError::BadLength));
        }
        let descriptor: [u8; DESCRIPTOR_LEN] = *image.last_chunk().expect("the image is longer than a descriptor");
        let parsed = read_descriptor_unverified(&descriptor).map_err(PlanningError::BadImage)?;
        if parsed.geometry_id != geometry.id() {
            return Err(PlanningError::GeometryMismatch);
        }
        let mut chunks = Vec::new();
        for data in image.chunks(geometry.chunk as usize) {
            let state = ChunkState::of(data);
            if state != ChunkState::Blank {
                store.put(data).map_err(PlanningError::Store)?;
            }
            chunks.push(state);
        }
        Ok(Self { descriptor, chunks })
    }

    /// The descriptor's fields (its seal is the device's to check).
    pub fn info(&self) -> ImageDescriptor {
        read_descriptor_unverified(&self.descriptor).expect("a release holds a descriptor that read when it was added")
    }
}

/// Choices the planner makes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Policy {
    /// Program chunks the device reports blank without erasing their block first. `None` picks by geometry:
    /// yes where a chunk is smaller than an erase unit (that is what makes a resume cheap there), no where a
    /// chunk is an erase unit (an erase cut by power loss can read blank and still be marginal; erasing again
    /// costs nothing on the wire).
    pub trust_blank: Option<bool>,
}

/// Something the operator must see.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Warning {
    /// The device accepts images without a signature: anyone who can talk to it can replace its firmware.
    UnsignedDevice,
}

/// What a plan costs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PlanStats {
    /// Chunks left as they are.
    pub skipped: u32,
    /// Chunks copied from the running image.
    pub copied: u32,
    /// Chunks sent over the wire.
    pub written: u32,
    /// Erase blocks erased.
    pub erased_blocks: u32,
    /// Size of the encoded plan.
    pub wire_bytes: usize,
    /// Size of the full image, for comparison.
    pub image_bytes: usize,
}

/// Why no plan was made.
#[derive(Debug)]
pub enum PlanningError {
    /// Device and release are for different flash layouts (`GEOMETRY_MISMATCH`).
    GeometryMismatch,
    /// The release is sealed in a way the device does not accept.
    SchemeMismatch,
    /// The manifest does not fit the geometry.
    BadManifest,
    /// The image is not a chunky image.
    BadImage(ImageError),
    /// The store lacks a chunk of the release.
    MissingBlob(ChunkHash),
    /// The store failed.
    Store(io::Error),
}

impl std::fmt::Display for PlanningError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            PlanningError::GeometryMismatch => {
                write!(f, "GEOMETRY_MISMATCH: device and release have different flash layouts")
            }
            PlanningError::SchemeMismatch => write!(f, "the release is sealed in a way the device does not accept"),
            PlanningError::BadManifest => write!(f, "the manifest does not fit the geometry"),
            PlanningError::BadImage(e) => write!(f, "not a chunky image: {e:?}"),
            PlanningError::MissingBlob(hash) => write!(f, "the store lacks chunk {hash:02x?}"),
            PlanningError::Store(e) => write!(f, "blob store: {e}"),
        }
    }
}

impl std::error::Error for PlanningError {}

/// One step of a plan, a WRITE holding its chunk.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PlanStep {
    /// Erase one erase block of DFU.
    Erase {
        /// Erase block index.
        block: u16,
    },
    /// Copy one chunk of the running image into DFU.
    Copy {
        /// Chunk of DFU.
        dst: u16,
        /// Chunk of ACTIVE.
        src: u16,
        /// Hash the source must have.
        hash: ChunkHash,
    },
    /// Program one chunk of DFU with `data`.
    Write {
        /// Chunk of DFU.
        dst: u16,
        /// Hash of `data`.
        hash: ChunkHash,
        /// The chunk.
        data: Vec<u8>,
    },
}

impl PlanStep {
    /// The step as `chunky_format` writes it.
    pub fn wire(&self) -> Step<'_> {
        match self {
            PlanStep::Erase { block } => Step::Erase { block: *block },
            PlanStep::Copy { dst, src, hash } => Step::Copy {
                dst: *dst,
                src: *src,
                hash: hash.0,
            },
            PlanStep::Write { dst, hash, data } => Step::Write {
                dst: *dst,
                data: List::new(data),
                hash: hash.0,
            },
        }
    }

    /// Wire form.
    pub fn encode(&self) -> Vec<u8> {
        let step = self.wire();
        let mut out = vec![0; step.head_tail_len()];
        step.to_head_tail(&mut out).expect("the buffer is the step's length");
        out
    }
}

/// A plan, ready to send.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Plan {
    /// The header.
    pub header: PlanHeader,
    /// The steps.
    pub steps: Vec<PlanStep>,
    /// What it costs.
    pub stats: PlanStats,
    /// What the operator must see.
    pub warnings: Vec<Warning>,
}

impl Plan {
    /// Wire form.
    pub fn encode(&self) -> Vec<u8> {
        let mut out = vec![0; PLAN_HEADER_LEN];
        self.header
            .to_fixed(&mut out)
            .expect("the buffer is the header's length");
        for step in &self.steps {
            out.extend_from_slice(&step.encode());
        }
        out
    }
}

/// Plans the update of the device behind `manifest` to `release`.
pub fn plan(
    geometry: &FlashGeometry,
    manifest: &Manifest,
    release: &Release,
    store: &impl BlobStore,
    policy: &Policy,
) -> Result<Plan, PlanningError> {
    let info = release.info();
    if manifest.header.geometry_id != geometry.id() || info.geometry_id != geometry.id() {
        return Err(PlanningError::GeometryMismatch);
    }
    let chunks = geometry.chunks() as usize;
    if manifest.active.len() != chunks || manifest.dfu.len() != chunks || release.chunks.len() != chunks {
        return Err(PlanningError::BadManifest);
    }
    if manifest.header.scheme != info.scheme {
        return Err(PlanningError::SchemeMismatch);
    }
    let trust_blank = policy.trust_blank.unwrap_or(geometry.chunk < geometry.erase);

    let mut sources: HashMap<ChunkHash, u16> = HashMap::new();
    for (index, state) in manifest.active.iter().enumerate() {
        if let ChunkState::Hash(hash) = state {
            sources.entry(*hash).or_insert(index as u16);
        }
    }

    let mut steps = Vec::new();
    let mut stats = PlanStats {
        image_bytes: geometry.active.len as usize,
        ..PlanStats::default()
    };
    let mut program = |index: usize, steps: &mut Vec<PlanStep>| -> Result<(), PlanningError> {
        let ChunkState::Hash(hash) = release.chunks[index] else {
            return Ok(());
        };
        let dst = index as u16;
        let source = if manifest.active[index] == ChunkState::Hash(hash) {
            Some(dst)
        } else {
            sources.get(&hash).copied()
        };
        match source {
            Some(src) => {
                steps.push(PlanStep::Copy { dst, src, hash });
                stats.copied += 1;
            }
            None => {
                let data = store
                    .get(&hash)
                    .map_err(PlanningError::Store)?
                    .ok_or(PlanningError::MissingBlob(hash))?;
                steps.push(PlanStep::Write { dst, hash, data });
                stats.written += 1;
            }
        }
        Ok(())
    };

    let per_block = geometry.chunks_per_block() as usize;
    for block in 0..geometry.blocks() as usize {
        let range = block * per_block..(block + 1) * per_block;
        let wrong: Vec<usize> = range
            .clone()
            .filter(|&c| manifest.dfu[c] != release.chunks[c])
            .collect();
        if wrong.is_empty() {
            continue;
        }
        let all_blank = wrong.iter().all(|&c| manifest.dfu[c] == ChunkState::Blank);
        if all_blank && trust_blank {
            for &index in &wrong {
                program(index, &mut steps)?;
            }
        } else {
            steps.push(PlanStep::Erase { block: block as u16 });
            stats.erased_blocks += 1;
            for index in range {
                program(index, &mut steps)?;
            }
        }
    }

    stats.skipped = chunks as u32 - stats.copied - stats.written;
    let body_len: usize = steps.iter().map(|step| step.wire().head_tail_len()).sum();
    stats.wire_bytes = PLAN_HEADER_LEN + body_len;
    let mut warnings = Vec::new();
    if manifest.unsigned() {
        warnings.push(Warning::UnsignedDevice);
    }
    Ok(Plan {
        header: PlanHeader {
            magic: PLAN_MAGIC,
            format: FORMAT,
            geometry_id: geometry.id(),
            step_count: steps.len() as u32,
            body_len: body_len as u32,
            descriptor: split_descriptor(&release.descriptor),
        },
        steps,
        stats,
        warnings,
    })
}
