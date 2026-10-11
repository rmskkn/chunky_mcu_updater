//! What the delta layer needs from a boot backend.
//!
//! The delta layer stages an image into DFU and does not care how the device switches to it. Three backends
//! cover our chips: embassy-boot swap (`chunky_swap`), hardware bank swap and the RP2350 boot ROM.

/// Where the device stands between two updates.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootState {
    /// The running image is confirmed. An update may be staged.
    Confirmed,
    /// The last update did not confirm and was reverted. An update may be staged.
    Reverted,
    /// An update is staged and waits for the reboot.
    Staged,
    /// The running image is on its trial boot: it must confirm, or the next reset reverts it.
    Trial,
}

impl BootState {
    /// True when DFU may be written: not while it holds the image to switch or revert to.
    pub fn may_stage(self) -> bool {
        matches!(self, BootState::Confirmed | BootState::Reverted)
    }
}

/// The application's handle on the boot backend.
pub trait BootBackend {
    /// Backend error.
    type Error;

    /// Where the device stands.
    fn boot_state(&mut self) -> Result<BootState, Self::Error>;

    /// The image in DFU is complete and verified: switch to it at the next reset.
    fn mark_staged(&mut self) -> Result<(), Self::Error>;

    /// The running image works: keep it.
    fn confirm(&mut self) -> Result<(), Self::Error>;
}
