//! chunky on the host: a NOR flash that behaves like the real families (write rules, torn writes and erases,
//! ECC faults) and a whole simulated device around it, for tests that cut power at every flash operation.
#![forbid(unsafe_code)]
#![warn(missing_docs)]

pub mod device;
pub mod flash;

pub use device::{Backend, Bank, Device, SimSwitch, Swap, bank_layout, layout};
pub use flash::{F1, FlashModel, G0, H7, H7Mini, Rp2350, Rule, SimError, SimFlash};
