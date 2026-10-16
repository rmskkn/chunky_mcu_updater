//! The bench firmware as the harness restores it before every run: the bootloader of the variant, the config
//! record, and snapshot S0 (STATE, ACTIVE and DFU as `mark_updated` left them, taken once by `snapshot`).
//!
//! Files in the firmware directory (`bench/fw/out`, HW-3):
//!
//! | File | What |
//! | --- | --- |
//! | `boot-stock.bin`, `boot-guarded.bin` | the bootloader builds, at most 24 KB |
//! | `app-a.bin`, `app-b.bin` | images A and B, padded to 32 KB here with `bench_layout::pad_image` if shorter |
//! | `s0/state.bin`, `s0/active.bin`, `s0/dfu.bin` | snapshot S0, written by `chunky-bench snapshot` |

use crate::Variant;
use anyhow::{Context, Result, bail};
use bench_layout::{ACTIVE, BOOTLOADER, CONFIG, Config, DFU, IMAGE_A, IMAGE_B, PAGE, Region, STATE, pad_image};
use std::path::Path;

/// Everything the harness writes into flash, and the two images it classifies against.
#[derive(Clone, Debug)]
pub struct Firmware {
    pub boot_stock: Vec<u8>,
    pub boot_guarded: Vec<u8>,
    /// Image A, a whole ACTIVE slot.
    pub app_a: Vec<u8>,
    /// Image B, a whole ACTIVE slot.
    pub app_b: Vec<u8>,
    /// Snapshot S0: STATE, ACTIVE and DFU, each a whole region.
    pub s0_state: Vec<u8>,
    pub s0_active: Vec<u8>,
    pub s0_dfu: Vec<u8>,
}

/// One region and the bytes it must hold.
#[derive(Clone, Debug)]
pub struct Target {
    pub region: Region,
    pub data: Vec<u8>,
}

fn read(dir: &Path, name: &str) -> Result<Vec<u8>> {
    let p = dir.join(name);
    std::fs::read(&p).with_context(|| format!("reading {} (bench/fw builds it; S0 comes from `snapshot`)", p.display()))
}

fn region_sized(mut data: Vec<u8>, region: Region, what: &str) -> Result<Vec<u8>> {
    if data.len() > region.len as usize {
        bail!(
            "{what} is {} bytes, more than its region ({} bytes)",
            data.len(),
            region.len
        );
    }
    data.resize(region.len as usize, 0xFF);
    Ok(data)
}

fn padded(raw: Vec<u8>, image: u8, what: &str) -> Result<Vec<u8>> {
    let mut buf = vec![0u8; ACTIVE.len as usize];
    if raw.len() == buf.len() {
        return Ok(raw);
    }
    if !pad_image(&mut buf[..], raw.len(), image) {
        bail!("{what} is {} bytes, more than ACTIVE ({} bytes)", raw.len(), ACTIVE.len);
    }
    buf[..raw.len()].copy_from_slice(&raw);
    Ok(buf)
}

impl Firmware {
    /// Loads the bootloaders and images, and S0 when `with_s0` (a `snapshot` run has none yet).
    pub fn load(dir: &Path, with_s0: bool) -> Result<Firmware> {
        let mut fw = Firmware {
            boot_stock: region_sized(read(dir, "boot-stock.bin")?, BOOTLOADER, "boot-stock.bin")?,
            boot_guarded: region_sized(read(dir, "boot-guarded.bin")?, BOOTLOADER, "boot-guarded.bin")?,
            app_a: padded(read(dir, "app-a.bin")?, IMAGE_A, "app-a.bin")?,
            app_b: padded(read(dir, "app-b.bin")?, IMAGE_B, "app-b.bin")?,
            s0_state: vec![],
            s0_active: vec![],
            s0_dfu: vec![],
        };
        if with_s0 {
            fw.s0_state = region_sized(read(dir, "s0/state.bin")?, STATE, "s0/state.bin")?;
            fw.s0_active = region_sized(read(dir, "s0/active.bin")?, ACTIVE, "s0/active.bin")?;
            fw.s0_dfu = region_sized(read(dir, "s0/dfu.bin")?, DFU, "s0/dfu.bin")?;
            if fw.s0_active != fw.app_a {
                bail!("s0/active.bin is not app-a.bin: images rebuilt since the snapshot? Take it again");
            }
            if fw.s0_dfu[..ACTIVE.len as usize] != fw.app_b[..] {
                bail!("s0/dfu.bin does not start with app-b.bin: images rebuilt since the snapshot? Take it again");
            }
        }
        Ok(fw)
    }

    /// Saves snapshot S0 next to the images.
    pub fn save_s0(dir: &Path, state: &[u8], active: &[u8], dfu: &[u8]) -> Result<()> {
        let s0 = dir.join("s0");
        std::fs::create_dir_all(&s0).with_context(|| format!("creating {}", s0.display()))?;
        for (name, data) in [("state.bin", state), ("active.bin", active), ("dfu.bin", dfu)] {
            std::fs::write(s0.join(name), data).with_context(|| format!("writing s0/{name}"))?;
        }
        Ok(())
    }

    /// Stand-in firmware for the fake bench and the tests: bootloaders that only carry their variant in the first
    /// byte (what [`crate::fake`] reads), filler-only images A and B, and S0 as embassy-boot's `mark_updated`
    /// leaves it (SWAP magic, progress valid and empty).
    pub fn synthetic() -> Firmware {
        let boot = |v: Variant| {
            let mut b = vec![0xFF; BOOTLOADER.len as usize];
            b[..4].copy_from_slice(&[v.layout() as u8, 0xB0, 0x07, 0x00]);
            b
        };
        let image = |id: u8| {
            let mut buf = vec![0u8; ACTIVE.len as usize];
            buf[..4].copy_from_slice(&[id, 0xA9, 0x9A, id]);
            pad_image(&mut buf, 4, id);
            buf
        };
        let (app_a, app_b) = (image(IMAGE_A), image(IMAGE_B));
        let mut s0_state = vec![0xFF; STATE.len as usize];
        s0_state[..8].fill(0xF0);
        let mut s0_dfu = app_b.clone();
        s0_dfu.resize(DFU.len as usize, 0xFF);
        Firmware {
            boot_stock: boot(Variant::Stock),
            boot_guarded: boot(Variant::Guarded),
            s0_active: app_a.clone(),
            app_a,
            app_b,
            s0_state,
            s0_dfu,
        }
    }

    /// Writes [`Firmware::synthetic`]'s files into `dir`, S0 included: a firmware directory for dry runs.
    pub fn write_synthetic(dir: &Path) -> Result<Firmware> {
        let fw = Firmware::synthetic();
        std::fs::create_dir_all(dir)?;
        for (name, data) in [
            ("boot-stock.bin", &fw.boot_stock),
            ("boot-guarded.bin", &fw.boot_guarded),
            ("app-a.bin", &fw.app_a),
            ("app-b.bin", &fw.app_b),
        ] {
            std::fs::write(dir.join(name), data)?;
        }
        Firmware::save_s0(dir, &fw.s0_state, &fw.s0_active, &fw.s0_dfu)?;
        Ok(fw)
    }

    pub fn bootloader(&self, v: Variant) -> &[u8] {
        match v {
            Variant::Stock => &self.boot_stock,
            Variant::Guarded => &self.boot_guarded,
        }
    }

    /// What flash must hold at the start of a run: bootloader, config, S0.
    pub fn targets(&self, v: Variant, config: &Config) -> Vec<Target> {
        let mut cfg = vec![0xFF; CONFIG.len as usize];
        cfg[..Config::BYTES].copy_from_slice(&config.encode());
        vec![
            Target {
                region: BOOTLOADER,
                data: self.bootloader(v).to_vec(),
            },
            Target {
                region: STATE,
                data: self.s0_state.clone(),
            },
            Target {
                region: CONFIG,
                data: cfg,
            },
            Target {
                region: ACTIVE,
                data: self.s0_active.clone(),
            },
            Target {
                region: DFU,
                data: self.s0_dfu.clone(),
            },
        ]
    }

    /// Only the bootloader of `v` and the config (for `measure` and provisioning: images stay as they are).
    pub fn boot_and_config(&self, v: Variant, config: &Config) -> Vec<Target> {
        self.targets(v, config)
            .into_iter()
            .filter(|t| t.region == BOOTLOADER || t.region == CONFIG)
            .collect()
    }
}

/// `data` cut into pages.
pub fn pages(data: &[u8]) -> Vec<&[u8]> {
    data.chunks(PAGE as usize).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn synthetic_round_trips_through_files() {
        let dir = tempfile::tempdir().unwrap();
        let fw = Firmware::write_synthetic(dir.path()).unwrap();
        let back = Firmware::load(dir.path(), true).unwrap();
        assert_eq!(back.app_a, fw.app_a);
        assert_eq!(back.s0_dfu, fw.s0_dfu);
        assert_eq!(pages(&fw.app_a).len(), 16);
        // A short image gets the filler.
        std::fs::write(dir.path().join("app-a.bin"), [1, 2, 3, 4, 5]).unwrap();
        let short = Firmware::load(dir.path(), false).unwrap();
        assert_eq!(short.app_a.len(), ACTIVE.len as usize);
        assert_eq!(&short.app_a[..8], &[1, 2, 3, 4, 5, 0xFF, 0xFF, 0xFF]);
        // ... and then S0 no longer matches it.
        assert!(Firmware::load(dir.path(), true).is_err());
        std::fs::write(dir.path().join("app-b.bin"), vec![0; 40_000]).unwrap();
        assert!(Firmware::load(dir.path(), false).is_err());
    }
}
