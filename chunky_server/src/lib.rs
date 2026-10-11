//! The host side of chunky: build and sign images, keep their chunks in a content-addressed store, and turn a
//! device manifest into a plan.
//!
//! The planner holds no secrets: it works on finished, sealed images. Signing ([`image::Signer`]) is a separate
//! step that belongs where the release key lives.
#![forbid(unsafe_code)]
#![warn(missing_docs)]

pub mod image;
pub mod keys;
pub mod manifest;
pub mod planner;
pub mod store;

pub use image::{ImageBuildError, ImageMeta, Signer, build_image};
pub use manifest::{Manifest, ManifestParseError};
pub use planner::{Plan, PlanStats, PlanningError, Policy, Release, Warning, plan};
pub use store::{BlobStore, DirStore, MemStore};
