//! Does a power cut during embassy-boot's erase of its STATE page lose the images?
//!
//! The same scenarios run against embassy-boot 0.7.0 (crates.io) and git master, stock (no wrapper) and with the
//! suggested fix (`ZeroMagicFirst`). Read `REPORT.md` next to the workspace for what the numbers mean.
//! `cargo test -p embassy_torn -- --nocapture` prints the tables.

use embassy_torn::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Scn {
    /// Trial boot not confirmed: the bootloader reverts, then clears STATE (revert path of `prepare_boot`).
    Revert,
    /// The application confirms the trial boot: `mark_booted` clears STATE (`set_magic`).
    Confirm,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Verdict {
    /// The cut never fired: the scenario ended before that operation.
    NotReached,
    /// Every boot after the cut ran a whole image and both images still exist in flash.
    Fine(Active),
    /// A boot returned with ACTIVE a mix of both images.
    Mixed(Vec<u8>),
    /// ACTIVE ok at each boot but one of the images is gone from flash altogether.
    Lost,
    /// `prepare_boot` returned an error: the device cannot boot (and will not recover by rebooting).
    BootError,
}

impl Verdict {
    fn bad(&self) -> bool {
        !matches!(self, Verdict::Fine(_) | Verdict::NotReached)
    }
}

#[derive(Default, Debug)]
struct Tally {
    fine_old: usize,
    fine_new: usize,
    mixed: usize,
    lost: usize,
    boot_error: usize,
    not_reached: usize,
    /// Lowest number of erased words (or lowest `erased` prefix) that produced a bad result.
    first_bad: Option<String>,
}

impl Tally {
    fn add(&mut self, v: &Verdict, what: impl FnOnce() -> String) {
        match v {
            Verdict::NotReached => self.not_reached += 1,
            Verdict::Fine(Active::Old) => self.fine_old += 1,
            Verdict::Fine(_) => self.fine_new += 1,
            Verdict::Mixed(_) => self.mixed += 1,
            Verdict::Lost => self.lost += 1,
            Verdict::BootError => self.boot_error += 1,
        }
        if v.bad() && self.first_bad.is_none() {
            self.first_bad = Some(format!("{} -> {v:?}", what()));
        }
    }
    fn bad(&self) -> usize {
        self.mixed + self.lost + self.boot_error
    }
}

macro_rules! suite {
    ($m:ident, $eb:ident) => {
        mod $m {
            use super::*;
            use embedded_storage::nor_flash::NorFlash;
            use $eb::{BlockingFirmwareState, BootLoader, BootLoaderConfig, State};

            fn boot<S: NorFlash>(f: &Shared, state: S) -> Result<State, ()> {
                let mut buf = [0u8; 16];
                BootLoader::new(BootLoaderConfig {
                    active: Part::active(f),
                    dfu: Part::dfu(f),
                    state,
                })
                .prepare_boot(&mut buf)
                .map_err(|_| ())
            }

            fn mark<S: NorFlash>(state: S, booted: bool) -> Result<(), ()> {
                let mut al = [0u8; WRITE];
                let mut fw = BlockingFirmwareState::new(state, &mut al);
                (if booted {
                    fw.mark_booted()
                } else {
                    fw.mark_updated()
                })
                .map_err(|_| ())
            }

            /// Runs the scenario with `cut` armed at the interesting step; returns the verdict and the flash.
            fn run<W: NorFlash>(scn: Scn, cut: Cut, wrap: &dyn Fn(Part) -> W) -> (Verdict, Shared) {
                let f = Flash::new();
                provision(&f);
                // Stage the update, swap in, new image is running its trial boot.
                mark(wrap(Part::state(&f)), false).unwrap();
                assert_eq!(boot(&f, wrap(Part::state(&f))), Ok(State::Swap));
                assert_eq!(active(&f), Active::New);
                f.borrow_mut().arm(cut);
                let _ = match scn {
                    Scn::Revert => boot(&f, wrap(Part::state(&f))).map(|_| ()),
                    Scn::Confirm => mark(wrap(Part::state(&f)), true),
                };
                if !f.borrow().fired {
                    return (Verdict::NotReached, f);
                }
                f.borrow_mut().reboot();
                // The application never confirms from here on; the third boot has settled everything.
                let mut verdict = None;
                let mut last = Active::Old;
                for _ in 0..4 {
                    match boot(&f, wrap(Part::state(&f))) {
                        Err(()) => {
                            verdict = Some(Verdict::BootError);
                            break;
                        }
                        Ok(_) => match active(&f) {
                            Active::Mixed(p) => {
                                verdict = Some(Verdict::Mixed(p));
                                break;
                            }
                            a => last = a,
                        },
                    }
                }
                let v = verdict.unwrap_or_else(|| {
                    if image_survives(&f, old_page) && image_survives(&f, new_page) {
                        Verdict::Fine(last)
                    } else {
                        Verdict::Lost
                    }
                });
                (v, f)
            }

            fn stock(p: Part) -> Part {
                p
            }
            fn fixed(p: Part) -> ZeroMagicFirst {
                ZeroMagicFirst {
                    inner: p,
                    mask_ecc: false,
                }
            }
            fn fixed_ecc(p: Part) -> ZeroMagicFirst {
                ZeroMagicFirst {
                    inner: p,
                    mask_ecc: true,
                }
            }

            fn words<W: NorFlash>(scn: Scn, wrap: &dyn Fn(Part) -> W) -> Tally {
                let mut t = Tally::default();
                let mut best_pop = u32::MAX;
                for mask in 0u32..1 << STATE_WORDS_USED {
                    let (v, _) = run(
                        scn,
                        Cut::StateErase {
                            nth: 0,
                            tear: Tear::Words(mask),
                        },
                        wrap,
                    );
                    if v.bad() && mask.count_ones() < best_pop {
                        best_pop = mask.count_ones();
                        t.first_bad = Some(format!("erased words {mask:#016b} -> {v:?}"));
                    }
                    let keep = t.first_bad.take();
                    t.add(&v, String::new);
                    t.first_bad = keep;
                }
                t
            }

            fn bits<W: NorFlash>(scn: Scn, wrap: &dyn Fn(Part) -> W) -> Tally {
                let mut t = Tally::default();
                for seed in 1..=3000u64 {
                    let (v, _) = run(
                        scn,
                        Cut::StateErase {
                            nth: 0,
                            tear: Tear::Bits(seed * 0x9E37_79B9),
                        },
                        wrap,
                    );
                    t.add(&v, || format!("bit tear seed {seed}"));
                }
                t
            }

            fn ecc<W: NorFlash>(scn: Scn, wrap: &dyn Fn(Part) -> W) -> Tally {
                let mut t = Tally::default();
                for erased in 0..STATE_WORDS_USED {
                    let (v, _) = run(
                        scn,
                        Cut::StateErase {
                            nth: 0,
                            tear: Tear::Ecc { erased },
                        },
                        wrap,
                    );
                    t.add(&v, || {
                        format!("ECC tear, {erased} words erased, next one unreadable")
                    });
                }
                t
            }

            /// Control: power lost cleanly before each single operation, and every torn program of one word.
            fn control<W: NorFlash>(scn: Scn, wrap: &dyn Fn(Part) -> W) -> (Tally, Vec<Reprogram>, Vec<Reprogram>) {
                let mut t = Tally::default();
                let mut reprogs = vec![];
                let mut torn_reprogs = vec![];
                for n in 0.. {
                    let (v, f) = run(scn, Cut::BeforeOp(n), wrap);
                    if v == Verdict::NotReached {
                        break;
                    }
                    t.add(&v, || format!("clean cut before op {n}"));
                    reprogs.extend(f.borrow().reprograms.iter().cloned());
                    // Torn program at op n (ignored when op n is an erase: reported as NotReached/clean).
                    for bytes in 1..0xFu8 {
                        let (v, f) = run(scn, Cut::TornProgram { nth: n, bytes }, wrap);
                        if v != Verdict::NotReached {
                            t.add(&v, || format!("torn program at op {n}, bytes {bytes:#06b}"));
                            torn_reprogs.extend(f.borrow().reprograms.iter().cloned());
                        }
                    }
                }
                (t, reprogs, torn_reprogs)
            }

            fn show(what: &str, scn: Scn, t: &Tally) {
                println!(
                    "{:>9} {:<7} {:<30} old {:>5} new {:>5} MIXED {:>5} LOST {:>5} BOOT-ERROR {:>3}  {}",
                    stringify!($m),
                    format!("{scn:?}"),
                    what,
                    t.fine_old,
                    t.fine_new,
                    t.mixed,
                    t.lost,
                    t.boot_error,
                    t.first_bad.as_deref().unwrap_or("")
                );
            }

            #[test]
            fn clean_cuts_and_torn_programs_are_safe() {
                for scn in [Scn::Revert, Scn::Confirm] {
                    let (t, reprogs, torn) = control(scn, &stock);
                    show("control (stock)", scn, &t);
                    assert_eq!(t.bad(), 0, "{:?}", t.first_bad);
                    // "Progress-invalid written twice": it happens. A clean cut between the invalidating write
                    // and the erase makes the next boot write the validity word again; always zeros over zeros.
                    // A torn program of a progress word is re-programmed whole (zeros over part-zeros).
                    println!(
                        "{:>9} {:<7} programs of non-erased words: {} after clean cuts, {} after torn programs",
                        stringify!($m),
                        format!("{scn:?}"),
                        reprogs.len(),
                        torn.len()
                    );
                    assert!(
                        reprogs.iter().all(|r| r.zeros && r.at == STATE_AT + WRITE),
                        "{reprogs:?}"
                    );
                    assert!(torn.iter().all(|r| r.zeros), "{torn:?}");
                    if scn == Scn::Revert {
                        assert!(
                            !reprogs.is_empty(),
                            "the second write of the progress-invalid word did not show"
                        );
                    }
                }
            }

            #[test]
            fn stock_torn_word_erase_corrupts() {
                let mut any = 0;
                for scn in [Scn::Revert, Scn::Confirm] {
                    let t = words(scn, &stock);
                    show("torn erase, words (stock)", scn, &t);
                    any += t.bad();
                }
                // This test documents the upstream bug: when embassy-boot fixes it, this assertion flips.
                assert!(
                    any > 0,
                    "no torn erase corrupted anything: the claim does not reproduce"
                );
            }

            #[test]
            fn stock_torn_bit_erase_corrupts() {
                let mut any = 0;
                for scn in [Scn::Revert, Scn::Confirm] {
                    let t = bits(scn, &stock);
                    show("torn erase, bits (stock)", scn, &t);
                    any += t.bad();
                }
                assert!(any > 0);
            }

            #[test]
            fn stock_ecc_magic_in_flight_cannot_boot() {
                for scn in [Scn::Revert, Scn::Confirm] {
                    let t = ecc(scn, &stock);
                    show("torn erase, ECC (stock)", scn, &t);
                    // Only the tear that leaves the magic word itself unreadable: every boot errors out.
                    assert_eq!(t.boot_error, 1, "{:?}", t.first_bad);
                    assert_eq!(t.mixed + t.lost, 0);
                }
            }

            fn dump(f: &Shared) -> String {
                let m = f.borrow();
                let page = |at: usize, n: usize| -> String {
                    (0..n)
                        .map(|p| format!("{:02X}", m.mem[at + p * ERASE]))
                        .collect::<Vec<_>>()
                        .join(" ")
                };
                let st: Vec<String> = (0..STATE_WORDS_USED)
                    .map(|w| format!("{:02X}", m.mem[STATE_AT + w * WRITE]))
                    .collect();
                format!(
                    "ACTIVE [{}]  DFU [{}]  STATE [{}]",
                    page(ACTIVE_AT, N),
                    page(DFU_AT, N + 1),
                    st.join(" ")
                )
            }

            /// The shortest failing sequence, printed step by step.
            #[test]
            fn minimal_sequence_trace() {
                for (scn, mask, expect) in [
                    (Scn::Revert, 0b0000_0000_0000_1010u32, vec![0xA0, 0xA1, 0xB2]),
                    (Scn::Confirm, 0b0000_0000_0000_0110u32, vec![0xA0, 0xA1, 0xB2]),
                ] {
                    let (v, f) = run(
                        scn,
                        Cut::StateErase {
                            nth: 0,
                            tear: Tear::Words(mask),
                        },
                        &stock,
                    );
                    println!(
                        "{} {scn:?}: torn erase of STATE, words erased {mask:#b} -> {v:?}",
                        stringify!($m)
                    );
                    println!("    end: {}", dump(&f));
                    assert_eq!(v, Verdict::Mixed(expect));
                }
                // Step by step for Revert, by hand.
                let f = Flash::new();
                provision(&f);
                mark(Part::state(&f), false).unwrap();
                println!("{}  1 mark_updated      : {}", stringify!($m), dump(&f));
                assert_eq!(boot(&f, Part::state(&f)), Ok(State::Swap));
                println!("{}  2 boot (swap)       : {}", stringify!($m), dump(&f));
                f.borrow_mut().arm(Cut::StateErase {
                    nth: 0,
                    tear: Tear::Words(0b1010),
                });
                assert!(boot(&f, Part::state(&f)).is_err());
                println!(
                    "{}  3 boot (revert), power lost in the STATE erase, words 1 and 3 erased:",
                    stringify!($m)
                );
                println!("                              {}", dump(&f));
                f.borrow_mut().reboot();
                let r = boot(&f, Part::state(&f));
                println!("{}  4 boot -> {r:?}: {}", stringify!($m), dump(&f));
            }

            #[test]
            fn zero_magic_first_survives_every_tear() {
                for scn in [Scn::Revert, Scn::Confirm] {
                    for (what, t) in [
                        ("words (fixed)", words(scn, &fixed)),
                        ("bits (fixed)", bits(scn, &fixed)),
                        ("ECC (fixed, masked)", ecc(scn, &fixed_ecc)),
                        ("control (fixed)", control(scn, &fixed).0),
                    ] {
                        show(what, scn, &t);
                        assert_eq!(t.bad(), 0, "{what}: {:?}", t.first_bad);
                    }
                }
            }
        }
    };
}

suite!(eb_0_7_0, eb07);
suite!(eb_master, ebmaster);

/// git master's `BootLoader::verify_update` rejects a bad signature by erasing STATE while it says SWAP.
/// Progress is zero there, so a torn erase cannot make a swap resume in the middle: checked here for every tear.
mod master_verify_update {
    use super::*;
    use ebmaster::{BlockingFirmwareState, BootLoader, BootLoaderConfig, State};

    // Compressed Ed25519 base point: a valid public key; the "signature" is whatever bytes sit in DFU.
    const KEY: [u8; 32] = [
        0x58, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
        0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
    ];

    fn loader(f: &Shared) -> BootLoader<Part, Part, Part> {
        BootLoader::new(BootLoaderConfig {
            active: Part::active(f),
            dfu: Part::dfu(f),
            state: Part::state(f),
        })
    }

    fn after_cut(f: &Shared) {
        f.borrow_mut().reboot();
        let mut buf = [0u8; 16];
        for _ in 0..3 {
            let st = loader(f)
                .verify_update(&mut buf, &KEY, ERASE as u32, 2 * ERASE as u32)
                .unwrap();
            let st2 = loader(f).prepare_boot(&mut buf).unwrap();
            assert!(
                matches!(st, State::Revert | State::Swap | State::Boot),
                "{st:?} {st2:?}"
            );
            // A rejected update must never be swapped in.
            assert_eq!(
                active(f),
                Active::Old,
                "after verify_update {st:?}, prepare_boot {st2:?}"
            );
        }
    }

    #[test]
    fn rejection_erase_is_safe_at_progress_zero() {
        let mut runs = 0;
        let mut cuts: Vec<Cut> = (0..8).map(Cut::BeforeOp).collect();
        cuts.extend((0..1u32 << 4).map(|m| Cut::StateErase {
            nth: 0,
            tear: Tear::Words(m),
        }));
        cuts.extend((0..4).map(|e| Cut::StateErase {
            nth: 0,
            tear: Tear::Ecc { erased: e },
        }));
        for cut in cuts {
            let f = Flash::new();
            provision(&f);
            let mut al = [0u8; WRITE];
            BlockingFirmwareState::new(Part::state(&f), &mut al)
                .mark_updated()
                .unwrap();
            f.borrow_mut().arm(cut);
            let mut buf = [0u8; 16];
            let r = loader(&f).verify_update(&mut buf, &KEY, ERASE as u32, 2 * ERASE as u32);
            if !f.borrow().fired {
                assert_eq!(r, Ok(State::Revert));
                continue;
            }
            runs += 1;
            // ECC: a fault on the magic word makes every boot error out (the same single case as above).
            if matches!(
                cut,
                Cut::StateErase {
                    tear: Tear::Ecc { erased: 0 },
                    ..
                }
            ) {
                f.borrow_mut().reboot();
                assert!(
                    loader(&f)
                        .verify_update(&mut buf, &KEY, ERASE as u32, 2 * ERASE as u32)
                        .is_err()
                );
                continue;
            }
            after_cut(&f);
        }
        println!("master verify_update, bad signature: {runs} cuts, ACTIVE stayed the old image every time");
    }
}
