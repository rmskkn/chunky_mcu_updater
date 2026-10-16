//! TIM2 (32-bit) as a free-running 1 MHz counter: busy waits and the erase time.

use embassy_stm32::pac;

/// SYSCLK and the timer clock after `embassy_stm32::init` with the default config: HSI16, no PLL, APB /1.
pub const TIMER_CLOCK_HZ: u32 = 16_000_000;

/// Starts TIM2 counting microseconds. Call once after `embassy_stm32::init`.
pub fn start() {
    pac::RCC.apbenr1().modify(|w| w.set_tim2en(true));
    let t = pac::TIM2;
    t.cr1().write(|w| w.set_cen(false));
    t.psc().write_value((TIMER_CLOCK_HZ / 1_000_000 - 1) as u16);
    t.arr().write_value(u32::MAX);
    // Load the prescaler now, not at the first overflow.
    t.egr().write(|w| w.set_ug(true));
    t.cr1().write(|w| w.set_cen(true));
}

/// Microseconds since [`start`], wrapping.
pub fn now_us() -> u32 {
    pac::TIM2.cnt().read()
}

/// Busy waits `us` microseconds.
pub fn delay_us(us: u32) {
    let t0 = now_us();
    while now_us().wrapping_sub(t0) < us {}
}

/// Busy waits `ms` milliseconds.
pub fn delay_ms(ms: u32) {
    for _ in 0..ms {
        delay_us(1000);
    }
}
