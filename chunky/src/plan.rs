//! The plan: what the server tells the device to do. Untrusted input. Format 2, the types of `chunky_format`
//! (whose crate docs have the layout).
//!
//! A 145-byte header ([`PlanHeader`]: magic `CKP2`, format, geometry id, step count, body length, the target's
//! sealed descriptor), then `step count` steps, `body length` bytes in all. A step is its op byte and the fixed
//! head of its op; a WRITE's data follows its head:
//!
//! | Op | Bytes | Fields |
//! |---|---|---|
//! | 1 ERASE | 3 | erase block of DFU |
//! | 2 COPY | 13 | chunk of DFU, chunk of ACTIVE, chunk hash |
//! | 3 WRITE | 13 + one chunk | chunk of DFU, data count, chunk hash, then the data |
//!
//! There are no reserved bytes and no optional fields, so there is one encoding per plan and nothing a reader
//! could read two ways. Steps are read with [`read_step`] (`Step::view`: the lengths checked, the fields read at
//! their offsets, a WRITE's data a slice of the step's bytes).

pub use chunky_format::{
    FormatError as PlanError, PLAN_MAGIC, PlanHeader, Step, StepView, read_header_fixed, read_step, step_len,
    step_len_from_head,
};

/// Size of the plan header.
pub const PLAN_HEADER_LEN: usize = PlanHeader::ENCODED_LEN;
/// Longest head of a step, op byte included: a receiver of a stream frames a step from this many bytes.
pub const MAX_STEP_HEAD_LEN: usize = 1 + Step::WRITE_HEAD_LEN;
