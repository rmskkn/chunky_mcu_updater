//! Delta firmware updates for small microcontrollers.
//!
//! The device reports a hash per flash chunk ([`manifest`]), the server answers with a plan of erase, copy and
//! write steps ([`plan`]), and the device applies it into its inactive area ([`apply`]). A boot backend
//! ([`backend`]) then switches to the new image, gives it one trial boot and reverts when it does not confirm.
//!
//! Everything a device accepts from outside is untrusted: see `SECURITY.md` in the repository.
#![no_std]
#![forbid(unsafe_code)]
#![warn(missing_docs)]

#[cfg(not(any(feature = "ed25519", feature = "unsigned")))]
compile_error!(
    "chunky: say how images are verified: feature `ed25519` (signed releases), or `unsigned` as the explicit \
     opt-out for small flash (integrity only: see SECURITY.md)"
);
#[cfg(all(target_os = "none", feature = "ed25519", feature = "unsigned"))]
compile_error!("chunky: a firmware build trusts one scheme: enable `ed25519` or `unsigned`, not both");

#[cfg(test)]
extern crate std;

pub mod apply;
pub mod backend;
pub mod chips;
pub mod descriptor;
pub mod ecc;
pub mod floor;
pub mod geometry;
pub mod hash;
pub mod manifest;
pub mod part;
pub mod plan;
pub mod series;

pub use apply::{Applier, ApplyError};
pub use backend::{BootBackend, BootState};
pub use descriptor::{DESCRIPTOR_LEN, Expect, ImageDescriptor, ImageError, PublicKey, Scheme, Trust, verify_image};
pub use ecc::{EccError, Masked, ReadFault, try_read};
pub use geometry::{BackendKind, EraseRun, Family, FlashGeometry, GeometryError, Region};
pub use hash::ChunkHash;
pub use manifest::ChunkState;
pub use part::{Part, PartError, ReadOnly};
pub use plan::{MAX_STEP_HEAD_LEN, PLAN_HEADER_LEN, PlanError, PlanHeader, Step, StepView};
