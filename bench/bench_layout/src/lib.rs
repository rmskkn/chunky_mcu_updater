//! Power-cut bench (HW-3, HW-4): the contract between the B129A test firmware (`bench/fw`) and the host
//! harness (`bench/host`). Flash and RAM layout, the config record the harness writes into flash, the report
//! record the firmware leaves in RAM, and the filler that makes every page of image A differ from image B.
//!
//! Everything sits in bank 1 of the STM32G0B1CE (512 KB, 2 KB pages, 8-byte write unit), so a STATE erase
//! stalls code fetches like it does with the production single-bank layout. See `bench/README.md`.
#![no_std]
#![forbid(unsafe_code)]

/// Erase page of the G0B1.
pub const PAGE: u32 = 2048;
/// Write unit (double word) of the G0B1, and embassy-boot's `WRITE_SIZE` for STATE.
pub const WRITE: u32 = 8;

/// A flash or RAM region: absolute address and length in bytes.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Region {
    pub at: u32,
    pub len: u32,
}

impl Region {
    pub const fn end(&self) -> u32 {
        self.at + self.len
    }
    pub const fn pages(&self) -> u32 {
        self.len / PAGE
    }
    /// Offset of this region from the start of flash (embassy-boot partitions take offsets).
    pub const fn offset(&self) -> u32 {
        self.at - FLASH_BASE
    }
}

pub const FLASH_BASE: u32 = 0x0800_0000;
/// The test bootloader (stock embassy-boot or with `chunky_swap::Guarded`), pages 0..12.
pub const BOOTLOADER: Region = Region {
    at: 0x0800_0000,
    len: 24 * 1024,
};
/// embassy-boot STATE: one page, so one erase is swept (as in `embassy_torn`).
pub const STATE: Region = Region {
    at: 0x0800_6000,
    len: PAGE,
};
/// The [`Config`] record, written by the harness, read by bootloader and application.
pub const CONFIG: Region = Region {
    at: 0x0800_6800,
    len: PAGE,
};
/// Erased again and again by the measure mode (same bank as STATE).
pub const SCRATCH: Region = Region {
    at: 0x0800_7000,
    len: PAGE,
};
/// ACTIVE: 16 pages; the application is linked here.
pub const ACTIVE: Region = Region {
    at: 0x0800_8000,
    len: 32 * 1024,
};
/// DFU: one page more than ACTIVE, as embassy-boot's swap needs.
pub const DFU: Region = Region {
    at: 0x0801_0000,
    len: 34 * 1024,
};

pub const RAM_BASE: u32 = 0x2000_0000;
/// SRAM of the G0B1 (144 KB); the last [`REPORT`] bytes are left out of the linker's RAM.
pub const RAM_LEN: u32 = 144 * 1024;
/// The [`Report`] record, not initialised by the runtime (it is reset by the bootloader on every boot).
pub const REPORT: Region = Region {
    at: RAM_BASE + RAM_LEN - 256,
    len: 256,
};

/// Image ids, also in the filler and in the report.
pub const IMAGE_A: u8 = 0xA;
pub const IMAGE_B: u8 = 0xB;

/// The 32-bit filler word at word `word` of page `page` of image `image`: different in every page of A and B,
/// so a page-wise mix of the two shows in a read-back.
pub const fn filler_word(image: u8, page: u32, word: u32) -> u32 {
    0xF000_0000 | (image as u32) << 24 | (page & 0xff) << 16 | (word & 0xffff)
}

/// Pads `image` (the application binary at its start, `len` bytes used) to the whole buffer with the filler.
/// The buffer is a whole slot (ACTIVE: 32 KB); the binary must fit. Returns false when it does not.
pub fn pad_image(buf: &mut [u8], len: usize, image: u8) -> bool {
    if len > buf.len() {
        return false;
    }
    let total = buf.len();
    let start = len.next_multiple_of(4).min(total);
    for b in &mut buf[len..start] {
        *b = 0xff;
    }
    let mut at = start;
    while at + 4 <= total {
        let w = filler_word(image, (at / PAGE as usize) as u32, ((at % PAGE as usize) / 4) as u32);
        buf[at..at + 4].copy_from_slice(&w.to_le_bytes());
        at += 4;
    }
    true
}

/// What the application does in this run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    /// Normal update run: B confirms (`mark_booted`) or resets unconfirmed, per [`Config::confirm`].
    Run = 0,
    /// Measurement: PA6 high, `pre_trigger_us`, erase [`SCRATCH`], PA6 low, every `period_ms`, forever.
    Measure = 1,
    /// Provisioning: A calls `mark_updated` (official API) and halts with a breakpoint, for the snapshot.
    Provision = 2,
}

/// Test config, little-endian words at [`CONFIG`]. A missing or broken record reads as [`Config::DEFAULT`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Config {
    pub mode: Mode,
    /// Image B calls `mark_booted` (true) or resets after `reset_after_ms` without confirming (false).
    pub confirm: bool,
    /// PA6 goes high this long before the STATE (or SCRATCH) erase starts, so the swept cut can land before
    /// the erase although the DUT's 3V3 holds up for a while after the cut.
    pub pre_trigger_us: u32,
    /// The bootloader waits this long before anything else: time for the harness to arm the glitcher.
    pub boot_delay_ms: u32,
    /// Unconfirmed B resets itself (SYSRESETREQ) after this long.
    pub reset_after_ms: u32,
    /// Measure mode: one erase every this many ms.
    pub period_ms: u32,
}

impl Config {
    pub const MAGIC: u32 = 0x4643_4243; // "CBCF"
    pub const VERSION: u32 = 1;
    pub const WORDS: usize = 8;
    pub const BYTES: usize = Self::WORDS * 4;

    pub const DEFAULT: Config = Config {
        mode: Mode::Run,
        confirm: true,
        pre_trigger_us: 0,
        boot_delay_ms: 0,
        reset_after_ms: 100,
        period_ms: 100,
    };

    fn words(&self) -> [u32; Self::WORDS] {
        let mut w = [
            Self::MAGIC,
            Self::VERSION | (self.mode as u32) << 16 | (self.confirm as u32) << 24,
            self.pre_trigger_us,
            self.boot_delay_ms,
            self.reset_after_ms,
            self.period_ms,
            0,
            0,
        ];
        w[7] = check(&w[..7]);
        w
    }

    /// The record as written to flash (a multiple of the write unit).
    pub fn encode(&self) -> [u8; Self::BYTES] {
        let mut out = [0u8; Self::BYTES];
        for (i, w) in self.words().iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        out
    }

    /// Parses the record; `None` for anything but a whole, valid record of this version.
    pub fn decode(bytes: &[u8]) -> Option<Config> {
        let mut w = [0u32; Self::WORDS];
        for (i, slot) in w.iter_mut().enumerate() {
            *slot = u32::from_le_bytes(bytes.get(i * 4..i * 4 + 4)?.try_into().ok()?);
        }
        if w[0] != Self::MAGIC || w[1] & 0xffff != Self::VERSION || w[7] != check(&w[..7]) || w[6] != 0 {
            return None;
        }
        let mode = match (w[1] >> 16) & 0xff {
            0 => Mode::Run,
            1 => Mode::Measure,
            2 => Mode::Provision,
            _ => return None,
        };
        let confirm = match w[1] >> 24 {
            0 => false,
            1 => true,
            _ => return None,
        };
        Some(Config {
            mode,
            confirm,
            pre_trigger_us: w[2],
            boot_delay_ms: w[3],
            reset_after_ms: w[4],
            period_ms: w[5],
        })
    }

    /// Like [`Config::decode`], [`Config::DEFAULT`] when there is no valid record.
    pub fn read_or_default(bytes: &[u8]) -> Config {
        Self::decode(bytes).unwrap_or(Self::DEFAULT)
    }
}

fn check(words: &[u32]) -> u32 {
    !words.iter().fold(0x5a5a_5a5a, |acc, w| acc.rotate_left(5) ^ w)
}

/// Which bootloader build ran.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Variant {
    /// embassy-boot 0.7.0 as it comes, default fault handlers.
    Stock = 1,
    /// STATE behind `chunky_swap::Guarded`, ECC faults caught by the NMI reader (ECC-2).
    Guarded = 2,
}

/// What `prepare_boot` returned, as the bootloader records it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BootState {
    /// The bootloader has not got that far.
    None = 0,
    Boot = 1,
    Swap = 2,
    Revert = 3,
    DfuDetach = 4,
    /// `prepare_boot` returned an error.
    Error = 0xEE,
}

/// How far the application got.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AppStage {
    NotStarted = 0,
    Running = 1,
    /// `mark_booted` returned Ok.
    Confirmed = 2,
    /// Unconfirmed, about to reset.
    Resetting = 3,
    /// `mark_booted` or `mark_updated` returned an error.
    Error = 0xEE,
}

/// The record at [`REPORT`], little-endian words: the bootloader resets it on every boot and fills its part,
/// the application fills the rest. The harness reads it after halting the core (with the core's IPSR, which
/// tells a hang in the NMI or HardFault handler apart from a running application).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Report {
    pub variant: u8,
    pub boot_state: u8,
    pub app_image: u8,
    pub app_stage: u8,
    /// ECC double errors the bootloader's or application's flash reader caught this boot (guarded only).
    pub ecc_faults: u32,
    /// Address of the last one.
    pub ecc_last_addr: u32,
    /// Duration of the last STATE or SCRATCH erase in µs, measured by the firmware (TIM, not the cut runs).
    pub erase_us: u32,
    /// Erases of STATE this boot (bootloader and application together).
    pub state_erases: u32,
}

impl Report {
    pub const MAGIC: u32 = 0x5052_4243; // "CBRP"
    pub const WORDS: usize = 8;
    pub const BYTES: usize = Self::WORDS * 4;

    pub const EMPTY: Report = Report {
        variant: 0,
        boot_state: BootState::None as u8,
        app_image: 0,
        app_stage: AppStage::NotStarted as u8,
        ecc_faults: 0,
        ecc_last_addr: 0,
        erase_us: 0,
        state_erases: 0,
    };

    pub fn encode(&self) -> [u8; Self::BYTES] {
        let w = [
            Self::MAGIC,
            u32::from_le_bytes([self.variant, self.boot_state, self.app_image, self.app_stage]),
            self.ecc_faults,
            self.ecc_last_addr,
            self.erase_us,
            self.state_erases,
            0,
            0,
        ];
        let mut out = [0u8; Self::BYTES];
        for (i, w) in w.iter().enumerate() {
            out[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        out
    }

    /// `None` when the magic is missing: nothing ran this boot (RAM after power-up is random).
    pub fn decode(bytes: &[u8]) -> Option<Report> {
        let word =
            |i: usize| -> Option<u32> { Some(u32::from_le_bytes(bytes.get(i * 4..i * 4 + 4)?.try_into().ok()?)) };
        if word(0)? != Self::MAGIC {
            return None;
        }
        let [variant, boot_state, app_image, app_stage] = word(1)?.to_le_bytes();
        Some(Report {
            variant,
            boot_state,
            app_image,
            app_stage,
            ecc_faults: word(2)?,
            ecc_last_addr: word(3)?,
            erase_us: word(4)?,
            state_erases: word(5)?,
        })
    }
}

// The layout holds together: page-aligned, in order, no overlap, DFU one page bigger, all in bank 1.
const _: () = {
    let order = [BOOTLOADER, STATE, CONFIG, SCRATCH, ACTIVE, DFU];
    let mut i = 0;
    while i < order.len() {
        assert!(order[i].at.is_multiple_of(PAGE) && order[i].len.is_multiple_of(PAGE));
        if i > 0 {
            assert!(order[i - 1].end() <= order[i].at);
        }
        i += 1;
    }
    assert!(DFU.len == ACTIVE.len + PAGE);
    assert!(DFU.end() <= FLASH_BASE + 256 * 1024);
    // embassy-boot STATE: magic, progress validity, then 4 progress words per ACTIVE page.
    assert!((2 + 4 * ACTIVE.pages()) * WRITE <= STATE.len);
    assert!(REPORT.end() == RAM_BASE + RAM_LEN && Report::BYTES as u32 <= REPORT.len);
    assert!((Config::BYTES as u32).is_multiple_of(WRITE));
};

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn config_round_trip() {
        let c = Config {
            mode: Mode::Measure,
            confirm: false,
            pre_trigger_us: 25_000,
            boot_delay_ms: 150,
            reset_after_ms: 80,
            period_ms: 200,
        };
        assert_eq!(Config::decode(&c.encode()), Some(c));
        assert_eq!(Config::decode(&Config::DEFAULT.encode()), Some(Config::DEFAULT));
    }

    #[test]
    fn config_rejects_garbage() {
        let good = Config::DEFAULT.encode();
        assert_eq!(Config::decode(&[0xff; 64]), None);
        assert_eq!(Config::decode(&[0; 64]), None);
        assert_eq!(Config::decode(&good[..31]), None);
        assert_eq!(Config::read_or_default(&[0xff; 32]), Config::DEFAULT);
        for byte in 0..good.len() {
            for bit in 0..8 {
                let mut bad = good;
                bad[byte] ^= 1 << bit;
                assert_eq!(Config::decode(&bad), None, "flip {byte}.{bit}");
            }
        }
        // Arbitrary bytes never panic.
        let mut x = 0x1234_5678u32;
        for _ in 0..10_000 {
            let mut b = [0u8; 40];
            for v in &mut b {
                x ^= x << 13;
                x ^= x >> 17;
                x ^= x << 5;
                *v = x as u8;
            }
            let _ = Config::decode(&b[..(x as usize % 41)]);
            let _ = Report::decode(&b[..(x as usize % 41)]);
        }
    }

    #[test]
    fn report_round_trip() {
        let r = Report {
            variant: Variant::Guarded as u8,
            boot_state: BootState::Revert as u8,
            app_image: IMAGE_A,
            app_stage: AppStage::Running as u8,
            ecc_faults: 2,
            ecc_last_addr: STATE.at + 8,
            erase_us: 22_010,
            state_erases: 1,
        };
        assert_eq!(Report::decode(&r.encode()), Some(r));
        assert_eq!(Report::decode(&[0; 32]), None);
    }

    #[test]
    fn filler_differs_per_page_and_image() {
        let mut a = [0u8; ACTIVE.len as usize];
        let mut b = [0u8; ACTIVE.len as usize];
        a[..5].copy_from_slice(b"hello");
        b[..5].copy_from_slice(b"hello");
        assert!(pad_image(&mut a, 5, IMAGE_A));
        assert!(pad_image(&mut b, 5, IMAGE_B));
        assert_eq!(&a[..8], b"hello\xff\xff\xff");
        for p in 0..ACTIVE.pages() as usize {
            let pa = &a[p * PAGE as usize..][..PAGE as usize];
            let pb = &b[p * PAGE as usize..][..PAGE as usize];
            assert_ne!(pa, pb, "page {p}");
            for q in 0..p {
                assert_ne!(pa, &a[q * PAGE as usize..][..PAGE as usize]);
            }
        }
        let too_long = a.len() + 1;
        assert!(!pad_image(&mut a, too_long, IMAGE_A));
    }
}
