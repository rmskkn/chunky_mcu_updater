//! [`Dut`] over probe-rs: Donguru's CMSIS-DAP v2 (`c0de:cafe`), STM32G0B1CEUx, SWD at up to 1 MHz. Attaches
//! without a reset; detaching drops the session, and Donguru floats SWD and CC.

use crate::dut::Dut;
use anyhow::{Context, Result, anyhow};
use probe_rs::flashing::DownloadOptions;
use probe_rs::probe::DebugProbeSelector;
use probe_rs::probe::list::Lister;
use probe_rs::{MemoryInterface, Permissions, Session};
use std::time::Duration;

/// Where the bench DUT is.
#[derive(Clone, Debug)]
pub struct ProbeConfig {
    /// `VID:PID[:serial]`.
    pub probe: String,
    /// probe-rs target name.
    pub chip: String,
    pub speed_khz: u32,
}

impl Default for ProbeConfig {
    fn default() -> Self {
        ProbeConfig {
            probe: "c0de:cafe".into(),
            chip: "STM32G0B1CEUx".into(),
            speed_khz: 1000,
        }
    }
}

pub struct ProbeDut {
    pub config: ProbeConfig,
    session: Option<Session>,
}

impl ProbeDut {
    pub fn new(config: ProbeConfig) -> ProbeDut {
        ProbeDut { config, session: None }
    }

    fn session(&mut self) -> Result<&mut Session> {
        self.session.as_mut().ok_or_else(|| anyhow!("not attached"))
    }
}

impl Dut for ProbeDut {
    fn attach(&mut self) -> Result<()> {
        if self.session.is_some() {
            return Ok(());
        }
        let selector: DebugProbeSelector = self
            .config
            .probe
            .parse()
            .with_context(|| format!("probe selector '{}'", self.config.probe))?;
        let mut probe = Lister::new()
            .open(selector)
            .with_context(|| format!("opening probe {}", self.config.probe))?;
        probe.set_speed(self.config.speed_khz)?;
        let session = probe
            .attach(self.config.chip.as_str(), Permissions::default())
            .with_context(|| format!("attaching to {}", self.config.chip))?;
        self.session = Some(session);
        Ok(())
    }

    fn detach(&mut self) {
        self.session = None;
    }

    fn read(&mut self, addr: u32, buf: &mut [u8]) -> Result<()> {
        let mut core = self.session()?.core(0)?;
        core.read(u64::from(addr), buf)?;
        Ok(())
    }

    fn read_u32(&mut self, addr: u32) -> Result<u32> {
        Ok(self.session()?.core(0)?.read_word_32(u64::from(addr))?)
    }

    fn write_u32(&mut self, addr: u32, value: u32) -> Result<()> {
        Ok(self.session()?.core(0)?.write_word_32(u64::from(addr), value)?)
    }

    fn program_pages(&mut self, pages: &[(u32, Vec<u8>)]) -> Result<()> {
        let session = self.session()?;
        let mut loader = session.target().flash_loader();
        for (at, data) in pages {
            loader.add_data(u64::from(*at), data)?;
        }
        let mut opts = DownloadOptions::default();
        opts.verify = true;
        loader.commit(session, opts)?;
        Ok(())
    }

    fn halt(&mut self) -> Result<u32> {
        let mut core = self.session()?.core(0)?;
        core.halt(Duration::from_millis(100))?;
        let psr = core.registers().psr().ok_or_else(|| anyhow!("core has no xPSR"))?.id();
        Ok(core.read_core_reg::<u32>(psr)?)
    }
}
