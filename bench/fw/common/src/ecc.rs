//! The G0 ECC NMI reader (ECC-2, the G0 half).
//!
//! On the G0 a read of a flash double word with an uncorrectable ECC error sets FLASH_ECCR.ECCD (ECCR2 for bank 2)
//! and raises an NMI; the load itself completes with unknown data. [`on_nmi`] records the fault, clears ECCD and
//! returns, and [`read`] tells the caller its copy hit one. A build that wants this behaviour defines the
//! `NonMaskableInt` handler and calls [`on_nmi`]; the stock bootloader does not, and hangs in cortex-m-rt's
//! default handler as an embassy-boot user's would.

use core::sync::atomic::{AtomicBool, Ordering};

use bench_layout::FLASH_BASE;
use embassy_stm32::pac;

/// Bank 2 starts here on the G0B1 (dual bank, 256 KB each).
const BANK2: u32 = FLASH_BASE + 256 * 1024;

static HIT: AtomicBool = AtomicBool::new(false);

/// The NMI handler body. Returns true when the NMI was an ECC double error (recorded and cleared); on false the
/// caller should hang, as the NMI has another cause (clock security, SRAM parity) nothing here handles.
pub fn on_nmi() -> bool {
    let mut ecc = false;
    let r1 = pac::FLASH.eccr().read();
    if r1.eccd() {
        // ADDR_ECC: double-word offset into the bank (RM0444 3.7.9).
        crate::report::ecc_fault(FLASH_BASE + r1.addr_ecc() as u32 * 8);
        // ECCD is write-1-to-clear; ECCC is left alone (write 0), and so is ECCIE.
        pac::FLASH.eccr().write(|w| {
            w.set_eccd(true);
            w.set_eccie(r1.eccie());
        });
        ecc = true;
    }
    let r2 = pac::FLASH.eccr2().read();
    if r2.eccd() {
        crate::report::ecc_fault(BANK2 + r2.addr_ecc() as u32 * 8);
        pac::FLASH.eccr2().write(|w| {
            w.set_eccd(true);
            w.set_eccie(r2.eccie());
        });
        ecc = true;
    }
    if ecc {
        HIT.store(true, Ordering::SeqCst);
    }
    ecc
}

/// Copies `out.len()` bytes of memory-mapped flash from `addr`. False when the copy hit an ECC double error
/// (the NMI ran during it); `out` is then unknown.
///
/// Without an NMI handler that calls [`on_nmi`] such a read never returns: the core hangs in the default handler.
pub fn read(addr: u32, out: &mut [u8]) -> bool {
    HIT.store(false, Ordering::SeqCst);
    for (i, b) in out.iter_mut().enumerate() {
        // SAFETY: the caller's range is inside the G0B1's flash (checked by `flash::Region`); flash is always
        // readable, and an ECC error is an NMI, not a bus fault.
        *b = unsafe { ((addr as usize + i) as *const u8).read_volatile() };
    }
    // The NMI is taken right after the faulting load; make sure it has run before we look.
    cortex_m::asm::dsb();
    cortex_m::asm::isb();
    !HIT.load(Ordering::SeqCst)
}
