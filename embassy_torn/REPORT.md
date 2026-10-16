# embassy-boot: a power cut during the STATE erase can lose both images

**Reproduces. Stock embassy-boot 0.7.0 (crates.io) and git master `34191ee2` (2026-10-11) behave the same.**
Code: `embassy_torn/` in this repository (`cargo test -p embassy_torn -- --nocapture --test-threads=1`).

## Versions

| | |
|---|---|
| embassy-boot 0.7.0 | crates.io, `=0.7.0` |
| embassy-boot git master | `embassy-rs/embassy` `34191ee2badce9d4ba63ac4545dc1d8abd1495eb`, committed 2026-10-11 03:50 UTC |

The revert code in `BootLoader::prepare_boot` and `set_magic` (mark_booted/mark_updated/mark_dfu) is
byte-for-byte the same in both. Master adds `BootLoader::verify_update` (see "Not affected").

## Flash model

`embassy_torn/src/lib.rs`, one flash array shared by three partitions, like a single-bank STM32:

- write unit 4 bytes, erase page 64 bytes; ACTIVE 3 pages, DFU 4 pages, STATE 1 page (16 words, 14 used).
- A program only clears bits (AND). An erase only sets bits (to 0xFF). Nothing else is ever changed.
- Power cut during **one erase of the STATE page**, three tear models, all "erase moves bits toward 1":
  - `Words(mask)`: every word of the page is either untouched or fully erased, any subset (all 2^14 subsets run).
  - `Bits(seed)`: per word untouched, fully erased, or some of its zero bits raised to one (3000 seeds).
  - `Ecc{erased}`: words `0..erased` erased, the next one unreadable (read returns an error until erased), the
    rest untouched (ECC flash: H7, G0 with ECC enabled).
- Programs are atomic per word; a separate control sweep also tears programs byte-wise and cuts cleanly before
  every single operation. Both are safe in stock embassy-boot (0 bad out of ~500 runs per scenario).
- Not modelled: erase order across several state pages (STATE here is one page), a program torn across words.

## Scenarios

Old image A (pages `A0 A1 A2`) in ACTIVE, new image B in DFU (`B0 B1 B2`), STATE erased.

1. `FirmwareState::mark_updated()` (state: magic SWAP), `prepare_boot` swaps; ACTIVE `B0 B1 B2`, DFU `B0 A0 A1 A2`.
2. **Revert**: the new image never confirms, the next `prepare_boot` reverts. Power is cut in the STATE erase of
   the revert path (`state.erase(0, capacity)` after the progress-invalid write, before `REVERT_MAGIC`).
3. **Confirm**: the new image calls `mark_booted()`; power is cut in `set_magic`'s `state.erase(0, capacity)`.
   (This path was not in the original claim and is the more common one.)
4. Reboot; run `prepare_boot` up to 4 times without confirming; after each, classify ACTIVE.

Expected: ACTIVE is A or B, whole, at every boot, and both images still exist in flash.

## Result

Counts are tear patterns (out of 16384 word tears, 3000 bit tears). "Mixed" = a boot returned with ACTIVE a
page-wise mixture of A and B. "Lost" = ACTIVE ok at each boot but a page of one image is gone from flash.

| scenario | tear | clean old / new | MIXED | LOST | boot error |
|---|---|---|---|---|---|
| Revert  | words | 12296 / 4 | **3728** | **356** | 0 |
| Revert  | bits  | 2646 / 0  | **267**  | **87**  | 0 |
| Confirm | words | 128 / 12288 | **3840** | **128** | 0 |
| Confirm | bits  | 78 / 2628 | **288**  | **6**   | 0 |
| both    | ECC   | 13 / 0, 0 / 13 | 0 | 0 | 1 of 14 (magic word unreadable) |

Identical for 0.7.0 and master. About a quarter of all word tears are bad.

## Shortest failing sequence (Revert, two words)

Tear: the **validity word (word 1) and the first swap-progress word index 1 (word 3)** erased, magic word (0)
and progress word 2 untouched. `cargo test -p embassy_torn minimal_sequence_trace -- --nocapture`:

```
 1 mark_updated      : ACTIVE [A0 A1 A2]  DFU [B0 B1 B2 FF]  STATE [F0 FF FF FF FF FF FF FF FF FF FF FF FF FF]
 2 boot (swap)       : ACTIVE [B0 B1 B2]  DFU [B0 A0 A1 A2]  STATE [F0 FF 00 00 00 00 00 00 FF FF FF FF FF FF]
 3 boot (revert) cut in state erase, words 1 and 3 erased:
                       ACTIVE [A0 A1 A2]  DFU [B0 B1 B2 A2]  STATE [F0 FF 00 FF 00 00 00 00 00 00 00 00 00 00]
 4 boot -> Ok(Swap)  : ACTIVE [A0 A1 B2]  DFU [B0 B1 B2 A2]  STATE [F0 FF 00 00 00 00 00 00 00 00 00 00 00 00]
```

(STATE shows the first byte of each word: `F0` magic SWAP, word 1 progress validity, then 2N swap and 2N revert
progress words, `00` = written.)

Step 3 is a finished revert, ACTIVE is A. The erase tears. Step 4: magic still SWAP; validity word reads
"valid" (erased); `current_progress` stops at the first erased word, index 1, so `is_swapped` is false and
`swap()` runs from step 1 over partitions that are already reverted: it copies DFU page 2 (B2) into ACTIVE
page 2 and carries on from there. ACTIVE `A0 A1 B2` is neither image, `prepare_boot` returns `State::Swap`, and
progress now reads complete. The device boots half of each firmware; the later revert works from this mix.

Confirm, same idea, tear = words 1 and 2 erased (mask `0b110`): ACTIVE ends `A0 A1 B2` and the magic reads
REVERT, i.e. a confirmed image is silently destroyed.

The bad tears are: magic word untouched (still SWAP), validity word erased (so progress is trusted), and the
swap-progress words only partly erased (so the resume point lands inside the finished swap/revert). Erased
magic reads as BOOT, and an unerased validity word reads "invalid = done": both are safe.

## ECC variant

If the word in flight when power is lost is the magic word itself, it faults on every read until it is
erased; `read_state` returns `Err` from every `prepare_boot`, forever (the bootloader cannot decide anything and
never gets to the erase). That is a brick of its own, not a lost image. Wrapped with "read errors read as zero"
it becomes a clean boot.

## Not affected

- `FirmwareUpdater::verify_and_mark_updated` in 0.7.0: on a bad signature it returns before touching STATE.
- git master `BootLoader::verify_update` rejects a bad signature with `state.erase(0, capacity)` while the magic
  says SWAP. That is only done at progress zero (`current_progress == 0`), so a torn erase leaves either SWAP
  with no progress (verify again, reject again) or BOOT; checked for 22 cuts including every tear of the first
  4 words and the ECC tears: ACTIVE stayed the old image every time (`master_verify_update` test).
- `mark_updated` / `mark_dfu` from BOOT/REVERT: the magic is not SWAP when the erase runs.
- Cutting power cleanly before any single operation, and byte-wise torn programs of any one word: safe.

## "Progress-invalid word written twice"

**Confirmed, but always zeros over zeros.** In `prepare_boot`'s revert path the invalidating write
(`state.write(WRITE_SIZE, zeros)`) is unconditional. If power is cut after it and before the erase lands, the
next boot sees SWAP with invalid progress (treated as "all done", so the revert copies are skipped), reaches the
same revert tail, and programs word 1 with zeros again: one second program of a non-erased word per such
reboot. `set_magic` checks the word first and does not do this. In the sweep every re-program of a non-erased
word (clean cuts: only word 1; torn programs: also half-written progress words that are written again) carries
all-zero data; none carries other data.
Whether that is acceptable is chip-specific and cannot be answered by this model: as far as I remember the
reference manuals (STM32F1, and G0/L4 for the double word) allow `0x0000` over a programmed location and fault on
anything else; H7 (ECC per flash word) is **not verified** (our tracker: ECC-2, needs hardware).

## Suggested fix

Zero the magic word before erasing STATE, wherever STATE is erased with the magic possibly SWAP
(`prepare_boot` revert tail, `set_magic`, and `verify_update` for consistency):

```rust
self.state.write(0, &[0u8; STATE::WRITE_SIZE])?;   // magic: not SWAP/REVERT/BOOT/DFU any more
self.state.erase(0, self.state.capacity() as u32)?;
```

`ZeroMagicFirst` in `embassy_torn/src/lib.rs` does exactly that as a wrapper around the state partition. Result
in the same sweeps, both versions: 0 bad of 16384 word tears, 3000 bit tears, 14 ECC tears (with reads of
unreadable words masked), and 0 bad with every clean cut and torn program of the zero write itself.
Why it works: after the zero write the magic word matches no state, so `read_state` returns `Boot` no matter
what the tear leaves in the progress words; a torn zero write leaves either the old magic with intact progress
(the situation before the cut) or a magic that is not exactly SWAP. The cost is one program of a non-erased
word with zeros (the same exception embassy-boot already relies on for the validity word).
