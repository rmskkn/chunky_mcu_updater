//! The bench bootloader (HW-3). embassy-boot 0.7.0's `prepare_boot`, then a jump to ACTIVE.
//!
//! `stock`: STATE is the plain flash region (behind the PA6 trigger), default fault handlers: an ECC double
//! error read hangs in the default NMI handler, as it would for an embassy-boot user. `guarded`: STATE behind
//! `chunky_swap::Guarded` (magic zeroed before every erase, ECC faults masked through the NMI reader). The trigger
//! sits on the raw erase in both, so both are cut against the same erase.
#![no_std]
#![no_main]

#[cfg(all(feature = "stock", feature = "guarded"))]
compile_error!("choose one of the features `stock` and `guarded`");
#[cfg(not(any(feature = "stock", feature = "guarded")))]
compile_error!("choose one of the features `stock` and `guarded`");

use bench_fw_common::flash::{Region, Shared};
use bench_fw_common::trigger::TriggerFlash;
use bench_fw_common::{read_config, report, timer};
use bench_layout::{ACTIVE, BootState, DFU, PAGE, Report, STATE, Variant};
use core::cell::RefCell;
use cortex_m_rt::entry;
use embassy_boot::{BootLoader, BootLoaderConfig, State};
use embassy_stm32::flash::Flash;
use embassy_stm32::gpio::{Level, Output, Speed};

#[cfg(feature = "stock")]
const VARIANT: Variant = Variant::Stock;
#[cfg(feature = "guarded")]
const VARIANT: Variant = Variant::Guarded;

#[cfg(feature = "guarded")]
#[cortex_m_rt::exception]
unsafe fn NonMaskableInt() {
    if !bench_fw_common::ecc::on_nmi() {
        bench_fw_common::hang();
    }
}

#[entry]
fn main() -> ! {
    let p = embassy_stm32::init(Default::default());
    // PA6 low before anything else: the glitcher triggers on its rising edge.
    let mut pin = Output::new(p.PA6, Level::Low, Speed::VeryHigh);
    // Status LED (PB14, high = on): solid on whenever the board has power.
    let led = Output::new(p.PB14, Level::High, Speed::Low);
    timer::start();
    report::write(&Report {
        variant: VARIANT as u8,
        ..Report::EMPTY
    });

    let flash: Shared = RefCell::new(Flash::new_blocking(p.FLASH));
    let config = read_config(&flash);
    // Time for the harness to arm the glitcher after power-up.
    timer::delay_ms(config.boot_delay_ms);

    let outcome = {
        let state = TriggerFlash::new(Region::new(&flash, STATE), &mut pin, config.pre_trigger_us, true);
        #[cfg(feature = "guarded")]
        let state = chunky_swap::Guarded::new(state);
        let mut bl = BootLoader::new(BootLoaderConfig {
            active: Region::new(&flash, ACTIVE),
            dfu: Region::new(&flash, DFU),
            state,
        });
        let mut buf = [0u8; PAGE as usize];
        match bl.prepare_boot(&mut buf) {
            Ok(State::Boot) => BootState::Boot,
            Ok(State::Swap) => BootState::Swap,
            Ok(State::Revert) => BootState::Revert,
            Ok(State::DfuDetach) => BootState::DfuDetach,
            Err(_) => BootState::Error,
        }
    };
    report::update(|r| r.boot_state = outcome as u8);
    if outcome == BootState::Error {
        // Stay here (thread mode) with the report saying why; a reset would hide the failure.
        bench_fw_common::hang();
    }
    // Keep PA6 driven low and the LED on into the application (dropping the drivers would float them).
    core::mem::forget(pin);
    core::mem::forget(led);
    // SAFETY: ACTIVE holds an application linked there (or whatever the cut left; the harness classifies that).
    unsafe {
        let cp = cortex_m::Peripherals::steal();
        cp.SCB.vtor.write(ACTIVE.at);
        cortex_m::asm::bootload(ACTIVE.at as *const u32)
    }
}
