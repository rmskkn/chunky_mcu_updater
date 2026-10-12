//! The bench application (HW-3), linked into ACTIVE. One source; the image id comes from the feature.
//!
//! - `Mode::Run`: image B confirms (`mark_booted`) when `confirm`, else resets after `reset_after_ms`; image A
//!   just runs.
//! - `Mode::Measure`: every `period_ms` one SCRATCH erase behind the PA6 trigger (M1, M2 of the procedure).
//! - `Mode::Provision`: `mark_updated`, then a breakpoint, so the harness can dump the snapshot S0.
//!
//! STATE goes through the same wrapper as in the bootloader that started us (the report says which):
//! `Guarded<TriggerFlash<..>>` after the guarded bootloader, `TriggerFlash<..>` after the stock one.
#![no_std]
#![no_main]

#[cfg(all(feature = "image-a", feature = "image-b"))]
compile_error!("choose one of the features `image-a` and `image-b`");
#[cfg(not(any(feature = "image-a", feature = "image-b")))]
compile_error!("choose one of the features `image-a` and `image-b`");

use bench_fw_common::flash::{Region, Shared};
use bench_fw_common::trigger::TriggerFlash;
use bench_fw_common::{read_config, report, timer};
#[cfg(feature = "image-a")]
use bench_layout::IMAGE_A;
use bench_layout::{AppStage, Config, IMAGE_B, Mode, SCRATCH, STATE, Variant, WRITE};
use core::cell::RefCell;
use cortex_m_rt::entry;
use embassy_boot::BlockingFirmwareState;
use embassy_stm32::flash::Flash;
use embassy_stm32::gpio::{Level, Output, Speed};
use embedded_storage::nor_flash::NorFlash;

#[cfg(feature = "image-a")]
const IMAGE: u8 = IMAGE_A;
#[cfg(feature = "image-b")]
const IMAGE: u8 = IMAGE_B;

#[cortex_m_rt::exception]
unsafe fn NonMaskableInt() {
    // After the stock bootloader an ECC NMI hangs here, as cortex-m-rt's default handler would.
    if report::variant() != Variant::Guarded as u8 || !bench_fw_common::ecc::on_nmi() {
        bench_fw_common::hang();
    }
}

#[derive(Clone, Copy)]
enum StateOp {
    MarkBooted,
    MarkUpdated,
}

fn state_op<S: NorFlash>(state: S, op: StateOp) -> bool {
    let mut aligned = [0u8; WRITE as usize];
    let mut fw = BlockingFirmwareState::new(state, &mut aligned);
    match op {
        StateOp::MarkBooted => fw.mark_booted().is_ok(),
        StateOp::MarkUpdated => fw.mark_updated().is_ok(),
    }
}

/// `op` on STATE through the wrapper of the bootloader variant this boot ran.
fn on_state(flash: &Shared, pin: &mut Output<'static>, config: &Config, op: StateOp) -> bool {
    let state = TriggerFlash::new(Region::new(flash, STATE), pin, config.pre_trigger_us, true);
    if report::variant() == Variant::Guarded as u8 {
        state_op(chunky_swap::Guarded::new(state), op)
    } else {
        state_op(state, op)
    }
}

/// Half period of the status LED (PB14): image A blinks slowly, image B fast.
const BLINK_MS: u32 = if IMAGE == IMAGE_B { 100 } else { 500 };

/// Blinks the status LED for good; the harness halts the core and reads the report and IPSR.
fn blink(led: &mut Output<'static>) -> ! {
    loop {
        led.toggle();
        timer::delay_ms(BLINK_MS);
    }
}

fn set_stage(stage: AppStage) {
    report::update(|r| r.app_stage = stage as u8);
}

#[entry]
fn main() -> ! {
    let p = embassy_stm32::init(Default::default());
    let mut pin = Output::new(p.PA6, Level::Low, Speed::VeryHigh);
    let mut led = Output::new(p.PB14, Level::Low, Speed::Low);
    timer::start();
    report::update(|r| {
        r.app_image = IMAGE;
        r.app_stage = AppStage::Running as u8;
    });

    let flash: Shared = RefCell::new(Flash::new_blocking(p.FLASH));
    let config = read_config(&flash);
    match config.mode {
        Mode::Run => {
            if IMAGE == IMAGE_B {
                if config.confirm {
                    let ok = on_state(&flash, &mut pin, &config, StateOp::MarkBooted);
                    set_stage(if ok { AppStage::Confirmed } else { AppStage::Error });
                } else {
                    timer::delay_ms(config.reset_after_ms);
                    set_stage(AppStage::Resetting);
                    cortex_m::peripheral::SCB::sys_reset();
                }
            }
            blink(&mut led)
        }
        Mode::Measure => {
            let mut scratch = TriggerFlash::new(Region::new(&flash, SCRATCH), &mut pin, config.pre_trigger_us, false);
            loop {
                let t0 = timer::now_us();
                // LED on over the erase, switched outside the PA6 edges so it adds nothing between them.
                led.set_high();
                if scratch.erase(0, SCRATCH.len).is_err() {
                    set_stage(AppStage::Error);
                }
                led.set_low();
                let period_us = config.period_ms.saturating_mul(1000);
                let spent = timer::now_us().wrapping_sub(t0);
                timer::delay_us(period_us.saturating_sub(spent));
            }
        }
        Mode::Provision => {
            let ok = on_state(&flash, &mut pin, &config, StateOp::MarkUpdated);
            set_stage(if ok { AppStage::Running } else { AppStage::Error });
            // The harness is attached and dumps S0 from here.
            cortex_m::asm::bkpt();
            blink(&mut led)
        }
    }
}
