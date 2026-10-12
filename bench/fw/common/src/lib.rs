//! Shared by the bench bootloader and application (HW-3, the G0 half of ECC-2).
//!
//! - [`flash::Region`]: one flash region of the contract (`bench_layout`) as a `NorFlash`, reads through the G0
//!   ECC NMI reader so an uncorrectable word comes back as an error for which `chunky::EccError::is_ecc` holds.
//! - [`trigger::TriggerFlash`]: PA6 high, `pre_trigger_us` busy wait, the raw erase, PA6 low; erase time and
//!   count into the report.
//! - [`ecc::on_nmi`]: the NMI handler body (a binary that wants it defines `NonMaskableInt` and calls it).
//! - [`report`]: the RAM record the harness reads after halting the core.
//! - [`timer`]: TIM2 as a free-running 1 MHz counter for busy waits and the erase time.
#![no_std]

pub mod ecc;
pub mod flash;
pub mod report;
pub mod timer;
pub mod trigger;

use bench_layout::{CONFIG, Config};
use embedded_storage::nor_flash::ReadNorFlash;

/// Reads the config record; [`Config::DEFAULT`] when it is missing, broken or faults on ECC.
pub fn read_config(flash: &flash::Shared) -> Config {
    let mut buf = [0u8; Config::BYTES];
    match flash::Region::new(flash, CONFIG).read(0, &mut buf) {
        Ok(()) => Config::read_or_default(&buf),
        Err(_) => Config::DEFAULT,
    }
}

/// Stays here for good; the harness halts the core and reads the report and IPSR.
pub fn hang() -> ! {
    loop {
        cortex_m::asm::nop();
    }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    // The report shows how far the boot got; a panic hangs in the context it ran in, not in a fault handler.
    hang()
}
