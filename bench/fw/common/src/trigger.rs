//! The PA6 trigger around the raw erase: the glitcher's time base for every cut.

use bench_layout::Report;
use embassy_stm32::gpio::Output;
use embedded_storage::nor_flash::{ErrorType, NorFlash, ReadNorFlash};

use crate::{report, timer};

/// A partition whose erases raise PA6 first: PA6 high, `pre_trigger_us` busy wait, the erase, PA6 low. The
/// erase time goes into the report (`erase_us`); erases of STATE are counted (`state_erases`).
///
/// PA6 is configured push-pull low as soon as the binary starts and stays low otherwise. The erase stalls code
/// fetch from bank 1, so nothing else can happen between the two edges.
pub struct TriggerFlash<'p, F> {
    inner: F,
    pin: &'p mut Output<'static>,
    pre_trigger_us: u32,
    is_state: bool,
}

impl<'p, F> TriggerFlash<'p, F> {
    /// `is_state`: count the erases in `Report::state_erases` (STATE; not SCRATCH).
    pub fn new(inner: F, pin: &'p mut Output<'static>, pre_trigger_us: u32, is_state: bool) -> Self {
        Self {
            inner,
            pin,
            pre_trigger_us,
            is_state,
        }
    }
}

impl<F: ErrorType> ErrorType for TriggerFlash<'_, F> {
    type Error = F::Error;
}

impl<F: ReadNorFlash> ReadNorFlash for TriggerFlash<'_, F> {
    const READ_SIZE: usize = F::READ_SIZE;

    fn read(&mut self, offset: u32, bytes: &mut [u8]) -> Result<(), Self::Error> {
        self.inner.read(offset, bytes)
    }

    fn capacity(&self) -> usize {
        self.inner.capacity()
    }
}

impl<F: NorFlash> NorFlash for TriggerFlash<'_, F> {
    const WRITE_SIZE: usize = F::WRITE_SIZE;
    const ERASE_SIZE: usize = F::ERASE_SIZE;

    fn erase(&mut self, from: u32, to: u32) -> Result<(), Self::Error> {
        self.pin.set_high();
        timer::delay_us(self.pre_trigger_us);
        let t0 = timer::now_us();
        let res = self.inner.erase(from, to);
        let took = timer::now_us().wrapping_sub(t0);
        self.pin.set_low();
        let is_state = self.is_state;
        report::update(|r: &mut Report| {
            r.erase_us = took;
            if is_state {
                r.state_erases = r.state_erases.wrapping_add(1);
            }
        });
        res
    }

    fn write(&mut self, offset: u32, bytes: &[u8]) -> Result<(), Self::Error> {
        self.inner.write(offset, bytes)
    }
}
