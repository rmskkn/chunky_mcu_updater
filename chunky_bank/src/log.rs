//! The state log: what the bank backend remembers between resets.
//!
//! The STATE partition is a row of records of [`BANK_RECORD_UNITS`] write units. Record 0 is the header: its
//! first unit holds a magic and the rest stays blank. Every other record is one update, a set of flags:
//!
//! | Unit | Flag | Written by | Means |
//! |---|---|---|---|
//! | 0 | staged | application | DFU holds a verified image: switch to it |
//! | 1 | to plain | bootloader | the switch goes to "swap bit clear" |
//! | 2 | to swapped | bootloader | the switch goes to "swap bit set" |
//! | 3 | tried | bootloader | the new image has had its trial boot |
//! | 4 | confirmed | application | the new image works: keep it |
//! | 5 | closed | bootloader | the update is over without a confirmation: refused or reverted |
//!
//! A flag is one write unit, blank until it is set by writing zeros, once. It reads as set when more than half
//! of its bits are zero, so a single flipped bit sets nothing, and a unit that faults on ECC reads as zeros
//! through [`Masked`] and counts as set. A write cut by power loss leaves a flag set or not set; whoever needs
//! it set writes zeros again, the one second write every family takes.
//!
//! Records are used in order and a new one starts only when the one before it is over, so the last used record
//! says where the device stands. Nothing is erased between updates.
//!
//! The partition is erased only by [`Log::format`], when every record is used up and the last one is over (or
//! the header is not there). An erase cut by power loss leaves every unit anywhere between old and blank, which
//! for a row of flags could read as anything, so the magic is overwritten with zeros first: whatever the erase
//! leaves behind is then not a log, and a partition without a log means "nothing in flight".

use chunky::ecc::{EccError, Masked};
use chunky::geometry::BANK_RECORD_UNITS;
use embedded_storage::nor_flash::{NorFlash, ReadNorFlash};

use crate::MAX_WRITE_SIZE;

/// The header's first unit is this, repeated. No byte of it is blank or zero.
const MAGIC: [u8; 8] = *b"CKYBANK1";

/// One flag of a record: its unit within the record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Flag {
    Staged = 0,
    ToPlain = 1,
    ToSwapped = 2,
    Tried = 3,
    Confirmed = 4,
    Closed = 5,
}

impl Flag {
    /// The flag that names `swapped` as the mapping an update switches to.
    pub(crate) fn target(swapped: bool) -> Self {
        if swapped { Flag::ToSwapped } else { Flag::ToPlain }
    }
}

/// Which mapping a record's update switches to.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Target {
    /// The bootloader has not started the switch.
    Unset,
    /// To this value of the swap bit.
    To(bool),
    /// Both flags are set: not something the bootloader writes.
    Garbage,
}

/// The flags of one record.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct Record {
    pub to_plain: bool,
    pub to_swapped: bool,
    pub tried: bool,
    pub confirmed: bool,
    pub closed: bool,
}

impl Record {
    /// True when the update is over: confirmed, refused or reverted.
    pub(crate) fn over(&self) -> bool {
        self.confirmed || self.closed
    }

    /// True when the update ended with the old image running again after a trial of the new one.
    pub(crate) fn reverted(&self) -> bool {
        self.closed && self.tried && !self.confirmed
    }

    pub(crate) fn target(&self) -> Target {
        match (self.to_plain, self.to_swapped) {
            (false, false) => Target::Unset,
            (true, false) => Target::To(false),
            (false, true) => Target::To(true),
            (true, true) => Target::Garbage,
        }
    }
}

/// What a scan of the log found.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct View {
    /// The header is there.
    pub valid: bool,
    /// The last used record and its index.
    pub last: Option<(u32, Record)>,
    /// The first unused record, if one is left.
    pub free: Option<u32>,
}

impl View {
    /// The update in flight, if there is one.
    pub(crate) fn open(&self) -> Option<(u32, Record)> {
        self.last.filter(|(_, record)| !record.over())
    }
}

/// The state partition as a log. Reads are masked: a unit that faults on ECC reads as zeros.
pub(crate) struct Log<S> {
    flash: Masked<S>,
}

impl<S> Log<S>
where
    S: NorFlash,
    S::Error: EccError,
{
    pub(crate) fn new(state: S) -> Self {
        assert!(
            S::WRITE_SIZE >= MAGIC.len() && S::WRITE_SIZE <= MAX_WRITE_SIZE,
            "chunky_bank: a write unit of 8 to 32 bytes"
        );
        Self { flash: Masked(state) }
    }

    fn unit_at(record: u32, unit: u32) -> u32 {
        (record * BANK_RECORD_UNITS + unit) * S::WRITE_SIZE as u32
    }

    fn records(&self) -> u32 {
        (self.flash.capacity() / (BANK_RECORD_UNITS as usize * S::WRITE_SIZE)) as u32
    }

    fn is_set(&mut self, record: u32, flag: Flag) -> Result<bool, S::Error> {
        let mut unit = [0; MAX_WRITE_SIZE];
        let unit = &mut unit[..S::WRITE_SIZE];
        self.flash.read(Self::unit_at(record, flag as u32), unit)?;
        let zeros: u32 = unit.iter().map(|byte| byte.count_zeros()).sum();
        Ok(zeros * 2 > S::WRITE_SIZE as u32 * 8)
    }

    fn valid(&mut self) -> Result<bool, S::Error> {
        let mut unit = [0; MAX_WRITE_SIZE];
        let unit = &mut unit[..S::WRITE_SIZE];
        self.flash.read(0, unit)?;
        Ok(unit.chunks(MAGIC.len()).all(|piece| piece == &MAGIC[..piece.len()]))
    }

    /// Reads where the device stands.
    pub(crate) fn scan(&mut self) -> Result<View, S::Error> {
        let mut view = View {
            valid: self.valid()?,
            last: None,
            free: None,
        };
        if !view.valid {
            return Ok(view);
        }
        let mut used = 0;
        for record in 1..self.records() {
            if !self.is_set(record, Flag::Staged)? {
                view.free = Some(record);
                break;
            }
            used = record;
        }
        if used != 0 {
            let flags = Record {
                to_plain: self.is_set(used, Flag::ToPlain)?,
                to_swapped: self.is_set(used, Flag::ToSwapped)?,
                tried: self.is_set(used, Flag::Tried)?,
                confirmed: self.is_set(used, Flag::Confirmed)?,
                closed: self.is_set(used, Flag::Closed)?,
            };
            view.last = Some((used, flags));
        }
        Ok(view)
    }

    /// Sets a flag. A flag that already reads as set is left alone.
    pub(crate) fn set(&mut self, record: u32, flag: Flag) -> Result<(), S::Error> {
        if self.is_set(record, flag)? {
            return Ok(());
        }
        self.flash.write(
            Self::unit_at(record, flag as u32),
            &[0; MAX_WRITE_SIZE][..S::WRITE_SIZE],
        )
    }

    /// Starts an empty log. Only for a partition with nothing in flight: see the module text.
    pub(crate) fn format(&mut self) -> Result<(), S::Error> {
        // Zeros go over anything on every family (on H7 the unit may fault afterwards; it reads as zeros
        // through the mask, and the erase clears it).
        self.flash.write(0, &[0; MAX_WRITE_SIZE][..S::WRITE_SIZE])?;
        let len = self.flash.capacity() as u32;
        self.flash.erase(0, len)?;
        let mut magic = [0; MAX_WRITE_SIZE];
        for piece in magic.chunks_mut(MAGIC.len()) {
            piece.copy_from_slice(&MAGIC[..piece.len()]);
        }
        self.flash.write(0, &magic[..S::WRITE_SIZE])
    }
}
