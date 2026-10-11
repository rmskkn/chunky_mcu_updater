//! The plan: what the server tells the device to do. Untrusted input.
//!
//! Wire format, little-endian. Header, 144 bytes: `CKPL`, format 1, 3 reserved, geometry id, step count, then
//! the 128-byte image descriptor of the target. Then `step count` steps of 24 bytes: op, reserved, `dst` (u16),
//! `src` (u16), reserved (u16), chunk hash (16). A WRITE step is followed by one chunk of data.
//!
//! | Op | `dst` | `src` | hash |
//! |---|---|---|---|
//! | 1 ERASE | erase block of DFU | 0 | zero |
//! | 2 COPY | chunk of DFU | chunk of ACTIVE | of the chunk |
//! | 3 WRITE | chunk of DFU | 0 | of the data that follows |
//!
//! Parsing is strict: unknown ops, non-zero reserved bytes and stray fields are errors, so there is one
//! encoding per plan and nothing a parser could read two ways.

use crate::descriptor::DESCRIPTOR_LEN;
use crate::hash::{ChunkHash, HASH_LEN};

/// Plan magic.
pub const MAGIC: [u8; 4] = *b"CKPL";
/// Plan format.
pub const FORMAT: u8 = 1;
/// Size of the plan header.
pub const PLAN_HEADER_LEN: usize = 16 + DESCRIPTOR_LEN;
/// Size of one step, without the data of a WRITE.
pub const STEP_LEN: usize = 8 + HASH_LEN;

/// A malformed plan.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PlanError {
    /// Not a plan.
    BadMagic,
    /// A format this code does not know.
    BadFormat,
    /// Reserved bytes or unused fields are not zero.
    Reserved,
    /// An op this code does not know.
    UnknownOp,
}

/// The plan header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PlanHeader {
    /// Geometry the plan was made for.
    pub geometry_id: u32,
    /// Steps that follow.
    pub step_count: u32,
    /// Descriptor of the image the plan produces, as it will lie at the end of DFU.
    pub descriptor: [u8; DESCRIPTOR_LEN],
}

impl PlanHeader {
    /// Wire form.
    pub fn encode(&self) -> [u8; PLAN_HEADER_LEN] {
        let mut out = [0; PLAN_HEADER_LEN];
        out[0..4].copy_from_slice(&MAGIC);
        out[4] = FORMAT;
        out[8..12].copy_from_slice(&self.geometry_id.to_le_bytes());
        out[12..16].copy_from_slice(&self.step_count.to_le_bytes());
        out[16..].copy_from_slice(&self.descriptor);
        out
    }

    /// Parses the wire form.
    pub fn decode(raw: &[u8; PLAN_HEADER_LEN]) -> Result<Self, PlanError> {
        if raw[0..4] != MAGIC {
            return Err(PlanError::BadMagic);
        }
        if raw[4] != FORMAT {
            return Err(PlanError::BadFormat);
        }
        if raw[5..8] != [0; 3] {
            return Err(PlanError::Reserved);
        }
        let mut descriptor = [0; DESCRIPTOR_LEN];
        descriptor.copy_from_slice(&raw[16..]);
        Ok(Self {
            geometry_id: u32::from_le_bytes([raw[8], raw[9], raw[10], raw[11]]),
            step_count: u32::from_le_bytes([raw[12], raw[13], raw[14], raw[15]]),
            descriptor,
        })
    }
}

/// One step of a plan.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
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
    /// Program one chunk of DFU with the data that follows the step.
    Write {
        /// Chunk of DFU.
        dst: u16,
        /// Hash the data must have.
        hash: ChunkHash,
    },
}

impl Step {
    /// Wire form.
    pub fn encode(&self) -> [u8; STEP_LEN] {
        let mut out = [0; STEP_LEN];
        let (op, dst, src, hash) = match *self {
            Step::Erase { block } => (1, block, 0, [0; HASH_LEN]),
            Step::Copy { dst, src, hash } => (2, dst, src, hash.0),
            Step::Write { dst, hash } => (3, dst, 0, hash.0),
        };
        out[0] = op;
        out[2..4].copy_from_slice(&dst.to_le_bytes());
        out[4..6].copy_from_slice(&src.to_le_bytes());
        out[8..].copy_from_slice(&hash);
        out
    }

    /// Parses the wire form.
    pub fn decode(raw: &[u8; STEP_LEN]) -> Result<Self, PlanError> {
        if raw[1] != 0 || raw[6..8] != [0; 2] {
            return Err(PlanError::Reserved);
        }
        let dst = u16::from_le_bytes([raw[2], raw[3]]);
        let src = u16::from_le_bytes([raw[4], raw[5]]);
        let mut hash = [0; HASH_LEN];
        hash.copy_from_slice(&raw[8..]);
        match raw[0] {
            1 if src != 0 || hash != [0; HASH_LEN] => Err(PlanError::Reserved),
            1 => Ok(Step::Erase { block: dst }),
            2 => Ok(Step::Copy {
                dst,
                src,
                hash: ChunkHash(hash),
            }),
            3 if src != 0 => Err(PlanError::Reserved),
            3 => Ok(Step::Write {
                dst,
                hash: ChunkHash(hash),
            }),
            _ => Err(PlanError::UnknownOp),
        }
    }

    /// True when a chunk of data follows the step on the wire.
    pub fn has_data(&self) -> bool {
        matches!(self, Step::Write { .. })
    }
}
