//! A whole simulated device on the swap backend: bootloader, application update client, and the flash under
//! both. Each method is one thing the real device does; a test strings them together and cuts power in between.

use std::cell::RefCell;

use chunky::descriptor::{ImageDescriptor, Trust};
use chunky::floor::read_floor;
use chunky::geometry::{BackendKind, FlashGeometry, Region};
use chunky::manifest::{ManifestError, write_manifest};
use chunky::plan::{PLAN_HEADER_LEN, STEP_LEN, Step};
use chunky::{Applier, ApplyError, BootBackend, BootState, Part, ReadOnly};
use chunky_swap::{BootConfig, BootFault, BootOutcome, MAX_WRITE_SIZE, SwapUpdater, boot};

use crate::flash::{FlashModel, SimFlash};

/// A swap-backend layout for model `M`: STATE, FLOOR, ACTIVE of `blocks` erase units, DFU one unit longer.
pub fn layout<M: FlashModel>(chunk: u32, blocks: u32) -> FlashGeometry {
    let erase = M::ERASE_SIZE as u32;
    let write = M::WRITE_SIZE as u32;
    let state_len = ((2 + 4 * blocks) * write).div_ceil(erase) * erase;
    let geometry = FlashGeometry {
        family: M::FAMILY,
        backend: BackendKind::Swap,
        chunk,
        erase,
        write,
        state: Region {
            offset: 0,
            len: state_len,
        },
        floor: Region {
            offset: state_len,
            len: erase,
        },
        active: Region {
            offset: state_len + erase,
            len: blocks * erase,
        },
        dfu: Region {
            offset: state_len + erase + blocks * erase,
            len: (blocks + 1) * erase,
        },
    };
    geometry
        .validate()
        .unwrap_or_else(|e| panic!("layout for {}: {e:?}", M::NAME));
    geometry
}

/// The simulated device.
pub struct Device<M: FlashModel> {
    /// The flash. Tests reach in to arm power cuts, inject faults and inspect.
    pub flash: RefCell<SimFlash<M>>,
    /// The layout.
    pub geometry: FlashGeometry,
    /// What the device's bootloader and application trust.
    pub trust: Trust,
    /// The device's product.
    pub product_id: u32,
}

impl<M: FlashModel> Device<M> {
    /// A device with erased flash.
    pub fn new(geometry: FlashGeometry, trust: Trust, product_id: u32) -> Self {
        let size = [geometry.state, geometry.floor, geometry.active, geometry.dfu]
            .iter()
            .map(|region| region.end() as usize)
            .max()
            .expect("four regions");
        Self {
            flash: RefCell::new(SimFlash::new(size)),
            geometry,
            trust,
            product_id,
        }
    }

    /// Factory programming: `image` goes into ACTIVE through the debug port.
    pub fn install(&self, image: &[u8]) {
        assert_eq!(image.len(), self.geometry.active.len as usize);
        self.flash
            .borrow_mut()
            .load(self.geometry.active.offset as usize, image);
    }

    /// A handle on one region, with full access: what a bootloader, or a compromised application, has.
    pub fn part(&self, region: Region) -> Part<'_, SimFlash<M>> {
        Part::new(&self.flash, region)
    }

    /// One reset: the bootloader runs and decides.
    pub fn boot(&self) -> Result<BootOutcome, BootFault> {
        let config = BootConfig {
            geometry: &self.geometry,
            trust: self.trust,
            product_id: self.product_id,
        };
        let mut buf = vec![0; self.geometry.chunk as usize];
        boot(
            &config,
            self.part(self.geometry.active),
            self.part(self.geometry.dfu),
            self.part(self.geometry.state),
            self.part(self.geometry.floor),
            &mut buf,
        )
    }

    /// The application hashes its flash.
    pub fn manifest(&self) -> Result<Vec<u8>, ManifestError> {
        let mut out = Vec::new();
        let mut buf = vec![0; self.geometry.chunk as usize];
        write_manifest(
            &self.geometry,
            &self.trust,
            &mut ReadOnly::new(self.part(self.geometry.active)),
            &mut ReadOnly::new(self.part(self.geometry.dfu)),
            &mut buf,
            |bytes| out.extend_from_slice(bytes),
        )?;
        Ok(out)
    }

    /// The version floor as the application reads it.
    pub fn floor(&self) -> u32 {
        let mut buf = [0; MAX_WRITE_SIZE];
        read_floor(&mut self.part(self.geometry.floor), M::WRITE_SIZE, &mut buf).expect("the floor reads")
    }

    /// The application applies a plan as it arrives and, when the image verifies, marks it staged.
    pub fn apply(&self, plan: &[u8]) -> Result<ImageDescriptor, ApplyError> {
        let chunk = self.geometry.chunk as usize;
        let mut buf = vec![0; chunk];
        let mut word = [0; MAX_WRITE_SIZE];
        let mut backend = SwapUpdater::new(self.part(self.geometry.state), &mut word[..M::WRITE_SIZE]);
        // The application's handles: ACTIVE read-only, DFU without the swap's spare page.
        let mut active = ReadOnly::new(self.part(self.geometry.active));
        let mut dfu = self.part(self.geometry.dfu).prefix(self.geometry.active.len);
        let floor = {
            let mut buf = [0; MAX_WRITE_SIZE];
            read_floor(
                &mut ReadOnly::new(self.part(self.geometry.floor)),
                M::WRITE_SIZE,
                &mut buf,
            )
            .map_err(|_| ApplyError::Flash)?
        };

        let (header, mut rest) = plan
            .split_first_chunk::<PLAN_HEADER_LEN>()
            .ok_or(ApplyError::Incomplete)?;
        let mut applier = Applier::begin(
            &self.geometry,
            header,
            &self.trust,
            self.product_id,
            floor,
            &mut backend,
        )?;
        while !rest.is_empty() {
            let (step, after) = rest.split_first_chunk::<STEP_LEN>().ok_or(ApplyError::Incomplete)?;
            rest = after;
            let data = match Step::decode(step) {
                Ok(step) if step.has_data() => {
                    let (data, after) = rest.split_at_checked(chunk).ok_or(ApplyError::Incomplete)?;
                    rest = after;
                    Some(data)
                }
                _ => None,
            };
            applier.step(step, data, &mut active, &mut dfu, &mut buf)?;
        }
        applier.finish(&mut dfu, &mut backend, &mut buf)
    }

    /// The application reports where it stands.
    pub fn boot_state(&self) -> Result<BootState, BootFault> {
        let mut word = [0; MAX_WRITE_SIZE];
        SwapUpdater::new(self.part(self.geometry.state), &mut word[..M::WRITE_SIZE]).boot_state()
    }

    /// The application confirms the running image.
    pub fn confirm(&self) -> Result<(), BootFault> {
        let mut word = [0; MAX_WRITE_SIZE];
        SwapUpdater::new(self.part(self.geometry.state), &mut word[..M::WRITE_SIZE]).confirm()
    }

    /// What a compromised application can do: ask for a swap without anything having been verified.
    pub fn mark_staged_unchecked(&self) -> Result<(), BootFault> {
        let mut word = [0; MAX_WRITE_SIZE];
        SwapUpdater::new(self.part(self.geometry.state), &mut word[..M::WRITE_SIZE]).mark_staged()
    }

    /// The raw bytes of a region, whatever the ECC state.
    pub fn dump(&self, region: Region) -> Vec<u8> {
        self.flash.borrow().dump(region.offset as usize, region.len as usize)
    }
}
