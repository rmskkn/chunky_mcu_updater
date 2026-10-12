//! embassy-boot 0.7.0's STATE page, decoded for the CSV: the magic word and the progress words.
//!
//! Layout (write unit 8 bytes on the G0): word 0 the magic, word 1 progress validity (erased = valid), words 2..
//! the swap progress (2 per ACTIVE page) and then the revert progress; a word counts once any byte of it is
//! written. `chunky_swap::Guarded` zeroes the magic before every erase, so `zero` is its mark.

use bench_layout::WRITE;
use std::fmt;

/// What the magic word holds.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Magic {
    /// 0xD0: boot ACTIVE.
    Boot,
    /// 0xF0: an update is staged (or a trial boot runs).
    Swap,
    /// 0xC0: reverted.
    Revert,
    /// 0xE0: DFU detach.
    DfuDetach,
    /// All 0xFF.
    Erased,
    /// All zeros (Guarded's neutralised magic).
    Zero,
    /// Anything else: a torn word.
    Other(u64),
    /// The debugger could not read it (ECC).
    Unreadable,
}

impl fmt::Display for Magic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Magic::Boot => f.write_str("boot"),
            Magic::Swap => f.write_str("swap"),
            Magic::Revert => f.write_str("revert"),
            Magic::DfuDetach => f.write_str("dfu"),
            Magic::Erased => f.write_str("erased"),
            Magic::Zero => f.write_str("zero"),
            Magic::Other(w) => write!(f, "{w:016x}"),
            Magic::Unreadable => f.write_str("ecc"),
        }
    }
}

/// The decoded STATE page.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StateWords {
    pub magic: Magic,
    /// Progress validity word erased (embassy-boot reads the progress only then).
    pub progress_valid: bool,
    /// embassy-boot's progress index: the first progress word with an erased byte.
    pub progress: usize,
    /// Progress words written in all (any byte not 0xFF), to see a torn erase.
    pub written: usize,
}

impl fmt::Display for StateWords {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{} p{}{} w{}",
            self.magic,
            self.progress,
            if self.progress_valid { "" } else { "!" },
            self.written
        )
    }
}

/// Decodes a STATE read-back; `unreadable` are the addresses of words the debugger could not read (relative to
/// the page). Never panics; a short page decodes what is there.
pub fn decode(page: &[u8], unreadable: &[u32]) -> StateWords {
    let w = WRITE as usize;
    let word = |i: usize| page.get(i * w..(i + 1) * w);
    let magic = if unreadable.iter().any(|&a| (a as usize) < w) {
        Magic::Unreadable
    } else {
        match word(0) {
            None => Magic::Unreadable,
            Some(m) if m.iter().all(|&b| b == 0xD0) => Magic::Boot,
            Some(m) if m.iter().all(|&b| b == 0xF0) => Magic::Swap,
            Some(m) if m.iter().all(|&b| b == 0xC0) => Magic::Revert,
            Some(m) if m.iter().all(|&b| b == 0xE0) => Magic::DfuDetach,
            Some(m) if m.iter().all(|&b| b == 0xFF) => Magic::Erased,
            Some(m) if m.iter().all(|&b| b == 0) => Magic::Zero,
            Some(m) => Magic::Other(u64::from_be_bytes(m.try_into().unwrap_or([0; 8]))),
        }
    };
    let progress_valid = word(1).is_some_and(|v| v.iter().all(|&b| b == 0xFF));
    let words = (page.len() / w).saturating_sub(2);
    let progress = (0..words)
        .position(|i| word(2 + i).is_some_and(|v| v.contains(&0xFF)))
        .unwrap_or(words);
    let written = (0..words)
        .filter(|&i| word(2 + i).is_some_and(|v| v.iter().any(|&b| b != 0xFF)))
        .count();
    StateWords {
        magic,
        progress_valid,
        progress,
        written,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn decodes() {
        let mut p = vec![0xFF; 2048];
        assert_eq!(decode(&p, &[]).to_string(), "erased p0 w0");
        p[..8].fill(0xF0);
        p[16..16 + 8 * 5].fill(0);
        let s = decode(&p, &[]);
        assert_eq!(
            (s.magic, s.progress, s.written, s.progress_valid),
            (Magic::Swap, 5, 5, true)
        );
        p[8..16].fill(0);
        p[200] = 0x7F;
        assert_eq!(decode(&p, &[]).to_string(), "swap p5! w6");
        p[..8].copy_from_slice(&[0, 0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0]);
        assert_eq!(decode(&p, &[]).magic.to_string(), "0000f0f0f0f0f0f0");
        assert_eq!(decode(&p, &[0]).magic, Magic::Unreadable);
        // Garbage and short pages never panic.
        for n in 0..20 {
            let _ = decode(&p[..n], &[1, 2, 3]);
        }
    }
}
