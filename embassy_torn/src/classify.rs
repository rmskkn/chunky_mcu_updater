//! What a power cut left in flash, from pages alone: ACTIVE and DFU as read back, against the pages of the old
//! and the new image. Pure functions on byte slices, shared by this crate's model (one fill byte per page) and the
//! hardware bench (`bench/host`, real 2 KB pages read over SWD), so both call the same outcome by the same name.

use std::fmt;

/// Where one page of ACTIVE came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PageOf {
    /// Page `n` of the old image (wherever it sits now).
    Old(usize),
    /// Page `n` of the new image.
    New(usize),
    /// All 0xFF.
    Erased,
    /// None of the above: torn, half programmed, or something else.
    Other,
}

impl fmt::Display for PageOf {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PageOf::Old(n) => write!(f, "A{n}"),
            PageOf::New(n) => write!(f, "B{n}"),
            PageOf::Erased => write!(f, "FF"),
            PageOf::Other => write!(f, "??"),
        }
    }
}

/// Which image ACTIVE holds.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Active {
    /// Every page of the old image, in place.
    Old,
    /// Every page of the new image, in place.
    New,
    /// Anything else: where each page came from, in order.
    Mixed(Vec<PageOf>),
}

/// What a read-back says as a whole.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Class {
    /// ACTIVE is the old image, and every page of both images is still somewhere in ACTIVE or DFU.
    Old,
    /// ACTIVE is the new image, both images complete.
    New,
    /// ACTIVE is a page-wise mixture (or holds pages of neither image).
    Mixed(Vec<PageOf>),
    /// ACTIVE is one whole image, but a page of one of them is gone from flash altogether.
    Lost,
}

impl Class {
    /// The short name the bench writes into its CSV: `old`, `new`, `mixed`, `lost`.
    pub fn name(&self) -> &'static str {
        match self {
            Class::Old => "old",
            Class::New => "new",
            Class::Mixed(_) => "mixed",
            Class::Lost => "lost",
        }
    }
}

impl fmt::Display for Class {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Class::Mixed(pages) => {
                write!(f, "mixed[")?;
                for (i, p) in pages.iter().enumerate() {
                    write!(f, "{}{p}", if i > 0 { " " } else { "" })?;
                }
                write!(f, "]")
            }
            c => f.write_str(c.name()),
        }
    }
}

fn find<P: AsRef<[u8]>>(page: &[u8], image: &[P]) -> Option<usize> {
    image.iter().position(|p| p.as_ref() == page)
}

/// Where `page` came from: the same position of either image first, then any page of them.
pub fn page_of<P: AsRef<[u8]>>(at: usize, page: &[u8], old: &[P], new: &[P]) -> PageOf {
    if old.get(at).is_some_and(|p| p.as_ref() == page) {
        PageOf::Old(at)
    } else if new.get(at).is_some_and(|p| p.as_ref() == page) {
        PageOf::New(at)
    } else if let Some(n) = find(page, old) {
        PageOf::Old(n)
    } else if let Some(n) = find(page, new) {
        PageOf::New(n)
    } else if page.iter().all(|&b| b == 0xFF) {
        PageOf::Erased
    } else {
        PageOf::Other
    }
}

/// Which image the pages of ACTIVE make up.
pub fn active<P: AsRef<[u8]>, Q: AsRef<[u8]>>(active: &[Q], old: &[P], new: &[P]) -> Active {
    let pages: Vec<PageOf> = active
        .iter()
        .enumerate()
        .map(|(i, p)| page_of(i, p.as_ref(), old, new))
        .collect();
    let whole = |want: fn(usize) -> PageOf, len: usize| {
        pages.len() == len && pages.iter().enumerate().all(|(i, &p)| p == want(i))
    };
    if whole(PageOf::Old, old.len()) {
        Active::Old
    } else if whole(PageOf::New, new.len()) {
        Active::New
    } else {
        Active::Mixed(pages)
    }
}

/// Whether every page of `image` still exists somewhere in ACTIVE or DFU (position does not matter: a swap
/// leaves the old image in DFU shifted by a page).
pub fn survives<P: AsRef<[u8]>, Q: AsRef<[u8]>>(image: &[P], active: &[Q], dfu: &[Q]) -> bool {
    image
        .iter()
        .all(|want| active.iter().chain(dfu).any(|p| p.as_ref() == want.as_ref()))
}

/// The whole read-back: [`active`], and for a whole image whether both images survive.
pub fn classify<P: AsRef<[u8]>, Q: AsRef<[u8]>>(active_pages: &[Q], dfu: &[Q], old: &[P], new: &[P]) -> Class {
    let whole = match active(active_pages, old, new) {
        Active::Mixed(p) => return Class::Mixed(p),
        Active::Old => Class::Old,
        Active::New => Class::New,
    };
    if survives(old, active_pages, dfu) && survives(new, active_pages, dfu) {
        whole
    } else {
        Class::Lost
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn img(base: u8, n: usize) -> Vec<Vec<u8>> {
        (0..n).map(|p| vec![base + p as u8; 8]).collect()
    }

    #[test]
    fn classes() {
        let (a, b) = (img(0xA0, 3), img(0xB0, 3));
        let mut dfu = b.clone();
        dfu.push(vec![0xFF; 8]);
        assert_eq!(classify(&a, &dfu, &a, &b), Class::Old);
        // After a swap: B in ACTIVE, A shifted by one page in DFU.
        let mut dfu_after = vec![vec![0xFF; 8]];
        dfu_after.extend(a.iter().cloned());
        assert_eq!(classify(&b, &dfu_after, &a, &b), Class::New);
        let mix = vec![b[0].clone(), a[1].clone(), vec![0x12; 8]];
        let c = classify(&mix, &dfu, &a, &b);
        assert_eq!(c, Class::Mixed(vec![PageOf::New(0), PageOf::Old(1), PageOf::Other]));
        assert_eq!(c.to_string(), "mixed[B0 A1 ??]");
        // A page of A in the wrong place is still A's page, and the order makes it a mixture.
        let moved = vec![a[1].clone(), a[1].clone(), a[2].clone()];
        assert_eq!(
            active(&moved, &a, &b),
            Active::Mixed(vec![PageOf::Old(1), PageOf::Old(1), PageOf::Old(2)])
        );
        // ACTIVE whole but B gone: lost.
        let mut dfu_lost = dfu.clone();
        dfu_lost[2] = vec![0xFF; 8];
        assert_eq!(classify(&a, &dfu_lost, &a, &b), Class::Lost);
        assert_eq!(page_of(0, &[0xFF; 8], &a, &b), PageOf::Erased);
        // Short read-back is never a whole image.
        assert!(matches!(active(&a[..2], &a, &b), Active::Mixed(_)));
    }
}
