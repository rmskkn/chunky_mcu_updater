//! A whole simulated device: bootloader, application update client, and the flash under both. Each method is
//! one thing the real device does; a test strings them together and cuts power in between.
//!
//! The device is generic over its boot backend: [`Swap`] (embassy-boot, the default) or [`Bank`] (hardware
//! bank swap). Everything above the backend (manifest, plan, apply) is the same code for both.

use std::cell::RefCell;
use std::fmt::Debug;
use std::marker::PhantomData;

use chunky::descriptor::{ImageDescriptor, Trust};
use chunky::floor::read_floor;
use chunky::geometry::{BackendKind, FlashGeometry, Region};
use chunky::manifest::{ManifestError, write_manifest};
use chunky::plan::{PLAN_HEADER_LEN, STEP_LEN, Step};
use chunky::{Applier, ApplyError, BootBackend, BootState, Part, ReadOnly};
use chunky_bank::{BankMap, BankParts, BankSwitch, BankUpdater};
use chunky_swap::{MAX_WRITE_SIZE, SwapUpdater};

use crate::flash::{FlashModel, SimError, SimFlash};

/// A boot backend the simulated device can be built on.
pub trait Backend<M: FlashModel>: Sized + 'static {
    /// What the bootloader decides.
    type Outcome: Debug;
    /// How the bootloader fails.
    type Fault: Debug;
    /// How the application's handle fails.
    type UpdaterError: Debug + PartialEq;
    /// The application's handle on the backend.
    type Updater<'a>: BootBackend<Error = Self::UpdaterError>;

    /// A layout for model `M` with an image of `blocks` erase units.
    fn layout(chunk: u32, blocks: u32) -> FlashGeometry;

    /// The flash of a new device with that layout.
    fn flash(geometry: &FlashGeometry) -> SimFlash<M>;

    /// Where STATE and FLOOR are addressed right now.
    fn state_and_floor(device: &Device<M, Self>) -> (Region, Region);

    /// One reset: the bootloader runs until it jumps or gives up.
    fn boot(device: &Device<M, Self>) -> Result<Self::Outcome, Self::Fault>;

    /// The image the bootloader jumps to and its state, or `None` when it does not jump.
    fn running(outcome: &Self::Outcome) -> Option<(ImageDescriptor, BootState)>;

    /// The application's handle. `word` is scratch of one write unit.
    fn updater<'a>(device: &'a Device<M, Self>, word: &'a mut [u8]) -> Self::Updater<'a>;
}

/// The embassy-boot swap backend (`chunky_swap`).
#[derive(Clone, Copy, Debug)]
pub struct Swap;

/// The hardware bank swap backend (`chunky_bank`).
#[derive(Clone, Copy, Debug)]
pub struct Bank;

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

/// A bank-swap layout for model `M`: two banks of a bootloader erase unit, an image of `blocks` erase units and
/// one more unit, STATE in the first bank and FLOOR in the second.
pub fn bank_layout<M: FlashModel>(chunk: u32, blocks: u32) -> FlashGeometry {
    let erase = M::ERASE_SIZE as u32;
    let bank = (blocks + 2) * erase;
    let geometry = FlashGeometry {
        family: M::FAMILY,
        backend: BackendKind::Bank,
        chunk,
        erase,
        write: M::WRITE_SIZE as u32,
        state: Region {
            offset: (blocks + 1) * erase,
            len: erase,
        },
        floor: Region {
            offset: bank + (blocks + 1) * erase,
            len: erase,
        },
        active: Region {
            offset: erase,
            len: blocks * erase,
        },
        dfu: Region {
            offset: bank + erase,
            len: blocks * erase,
        },
    };
    geometry
        .validate()
        .unwrap_or_else(|e| panic!("bank layout for {}: {e:?}", M::NAME));
    geometry
}

/// The SWAP_BANK option bit of a simulated flash.
pub struct SimSwitch<'a, M>(pub &'a RefCell<SimFlash<M>>);

impl<M: FlashModel> BankSwitch for SimSwitch<'_, M> {
    type Error = SimError;

    fn swapped(&mut self) -> Result<bool, SimError> {
        Ok(self.0.borrow().swapped())
    }

    fn set_swapped(&mut self, swapped: bool) -> Result<(), SimError> {
        self.0.borrow_mut().program_swap(swapped)
    }
}

impl<M: FlashModel> Backend<M> for Swap {
    type Outcome = chunky_swap::BootOutcome;
    type Fault = chunky_swap::BootFault;
    type UpdaterError = chunky_swap::BootFault;
    type Updater<'a> = SwapUpdater<'a, Part<'a, SimFlash<M>>>;

    fn layout(chunk: u32, blocks: u32) -> FlashGeometry {
        layout::<M>(chunk, blocks)
    }

    fn flash(geometry: &FlashGeometry) -> SimFlash<M> {
        let size = [geometry.state, geometry.floor, geometry.active, geometry.dfu]
            .iter()
            .map(|region| region.end() as usize)
            .max()
            .expect("four regions");
        SimFlash::new(size)
    }

    fn state_and_floor(device: &Device<M, Self>) -> (Region, Region) {
        (device.geometry.state, device.geometry.floor)
    }

    fn boot(device: &Device<M, Self>) -> Result<Self::Outcome, Self::Fault> {
        device.flash.borrow_mut().reset();
        let config = chunky_swap::BootConfig {
            geometry: &device.geometry,
            trust: device.trust,
            product_id: device.product_id,
        };
        let mut buf = vec![0; device.geometry.chunk as usize];
        chunky_swap::boot(
            &config,
            device.part(device.geometry.active),
            device.part(device.geometry.dfu),
            device.part(device.geometry.state),
            device.part(device.geometry.floor),
            &mut buf,
        )
    }

    fn running(outcome: &Self::Outcome) -> Option<(ImageDescriptor, BootState)> {
        match *outcome {
            chunky_swap::BootOutcome::Run { image, state, .. } => Some((image, state)),
            chunky_swap::BootOutcome::Recovery { .. } => None,
        }
    }

    fn updater<'a>(device: &'a Device<M, Self>, word: &'a mut [u8]) -> Self::Updater<'a> {
        SwapUpdater::new(device.part(device.geometry.state), word)
    }
}

impl<M: FlashModel> Backend<M> for Bank {
    type Outcome = chunky_bank::BootOutcome;
    type Fault = chunky_bank::BootFault;
    type UpdaterError = chunky_bank::UpdaterError;
    type Updater<'a> = BankUpdater<Part<'a, SimFlash<M>>>;

    fn layout(chunk: u32, blocks: u32) -> FlashGeometry {
        bank_layout::<M>(chunk, blocks)
    }

    /// Two banks, the same bootloader at the start of each.
    fn flash(geometry: &FlashGeometry) -> SimFlash<M> {
        let bank = geometry.bank_len() as usize;
        let mut flash = SimFlash::with_banks(bank);
        let map = BankMap::new(geometry, false);
        let loader: Vec<u8> = (0..map.loader.len).map(|i| (i * 31 + 7) as u8).collect();
        flash.load(map.loader.offset as usize, &loader);
        flash.load(map.other_loader.offset as usize, &loader);
        flash
    }

    fn state_and_floor(device: &Device<M, Self>) -> (Region, Region) {
        let map = device.bank_map();
        (map.state, map.floor)
    }

    /// The bootloader runs, and runs again after every reset it asks for.
    fn boot(device: &Device<M, Self>) -> Result<Self::Outcome, Self::Fault> {
        for _ in 0..4 {
            match device.boot_once()? {
                chunky_bank::BootOutcome::Reset => {}
                outcome => return Ok(outcome),
            }
        }
        panic!("the bootloader keeps resetting");
    }

    fn running(outcome: &Self::Outcome) -> Option<(ImageDescriptor, BootState)> {
        match *outcome {
            chunky_bank::BootOutcome::Run { image, state, .. } => Some((image, state)),
            chunky_bank::BootOutcome::Reset | chunky_bank::BootOutcome::Recovery { .. } => None,
        }
    }

    fn updater<'a>(device: &'a Device<M, Self>, _word: &'a mut [u8]) -> Self::Updater<'a> {
        BankUpdater::new(device.part(device.bank_map().state))
    }
}

impl<M: FlashModel> Device<M, Bank> {
    /// Where the partitions are addressed right now.
    pub fn bank_map(&self) -> BankMap {
        BankMap::new(&self.geometry, self.flash.borrow().swapped())
    }

    /// One reset and one run of the bootloader, which may end in [`chunky_bank::BootOutcome::Reset`].
    pub fn boot_once(&self) -> Result<chunky_bank::BootOutcome, chunky_bank::BootFault> {
        self.flash.borrow_mut().reset();
        let config = chunky_bank::BootConfig {
            geometry: &self.geometry,
            trust: self.trust,
            product_id: self.product_id,
        };
        let map = self.bank_map();
        let mut buf = vec![0; self.geometry.chunk as usize];
        chunky_bank::boot(
            &config,
            BankParts {
                active: ReadOnly::new(self.part(map.active)),
                dfu: ReadOnly::new(self.part(map.dfu)),
                state: self.part(map.state),
                floor: self.part(map.floor),
                loader: ReadOnly::new(self.part(map.loader)),
                other_loader: ReadOnly::new(self.part(map.other_loader)),
            },
            &mut SimSwitch(&self.flash),
            &mut buf,
        )
    }
}

/// The simulated device.
pub struct Device<M: FlashModel, B = Swap> {
    /// The flash. Tests reach in to arm power cuts, inject faults and inspect.
    pub flash: RefCell<SimFlash<M>>,
    /// The layout.
    pub geometry: FlashGeometry,
    /// What the device's bootloader and application trust.
    pub trust: Trust,
    /// The device's product.
    pub product_id: u32,
    backend: PhantomData<B>,
}

impl<M: FlashModel, B: Backend<M>> Device<M, B> {
    /// A device with erased flash.
    pub fn new(geometry: FlashGeometry, trust: Trust, product_id: u32) -> Self {
        Self {
            flash: RefCell::new(B::flash(&geometry)),
            geometry,
            trust,
            product_id,
            backend: PhantomData,
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
    pub fn boot(&self) -> Result<B::Outcome, B::Fault> {
        B::boot(self)
    }

    /// One reset, for code that only asks what runs afterwards: the image and its state, `None` when the
    /// bootloader does not jump, an error when it failed (power was cut).
    pub fn boot_running(&self) -> Result<Option<(ImageDescriptor, BootState)>, B::Fault> {
        self.boot().map(|outcome| B::running(&outcome))
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
        let (_, floor) = B::state_and_floor(self);
        read_floor(&mut self.part(floor), M::WRITE_SIZE, &mut buf).expect("the floor reads")
    }

    /// The application applies a plan as it arrives and, when the image verifies, marks it staged.
    pub fn apply(&self, plan: &[u8]) -> Result<ImageDescriptor, ApplyError> {
        let chunk = self.geometry.chunk as usize;
        let mut buf = vec![0; chunk];
        let mut word = [0; MAX_WRITE_SIZE];
        let mut backend = B::updater(self, &mut word[..M::WRITE_SIZE]);
        // The application's handles: ACTIVE read-only, DFU without the swap's spare page.
        let mut active = ReadOnly::new(self.part(self.geometry.active));
        let mut dfu = self.part(self.geometry.dfu).prefix(self.geometry.active.len);
        let floor = {
            let mut buf = [0; MAX_WRITE_SIZE];
            read_floor(
                &mut ReadOnly::new(self.part(B::state_and_floor(self).1)),
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
    pub fn boot_state(&self) -> Result<BootState, B::UpdaterError> {
        let mut word = [0; MAX_WRITE_SIZE];
        B::updater(self, &mut word[..M::WRITE_SIZE]).boot_state()
    }

    /// The application confirms the running image.
    pub fn confirm(&self) -> Result<(), B::UpdaterError> {
        let mut word = [0; MAX_WRITE_SIZE];
        B::updater(self, &mut word[..M::WRITE_SIZE]).confirm()
    }

    /// What a compromised application can do: ask for a switch without anything having been verified.
    pub fn mark_staged_unchecked(&self) -> Result<(), B::UpdaterError> {
        let mut word = [0; MAX_WRITE_SIZE];
        B::updater(self, &mut word[..M::WRITE_SIZE]).mark_staged()
    }

    /// The raw bytes of a region, whatever the ECC state.
    pub fn dump(&self, region: Region) -> Vec<u8> {
        self.flash.borrow().dump(region.offset as usize, region.len as usize)
    }
}
