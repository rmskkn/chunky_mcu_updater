# Measurements, 12 Oct 2026 (B129A, default brown-out level, HSI16)

Bench: glitcher GLT-10 (trigger H6, power out H8, marker G8), Donguru 0.4.0, M2K (A1 3V3, A2 5V0, DIO0 PA6,
DIO1 power out, DIO2 marker).

## M0 static

Passed (checked by hand): DUT off, 3V3 falls to 0 and stays; no back-power after the move to H6/H8/G8.

## M1 erase time

Stock bootloader + image A, `Mode::Measure`, `pre_trigger_us = 0`, `period_ms = 100`, flashed with probe-rs over
Donguru, reset over SWD (no power cycle). M2K logic, DIO0, 1 MS/s, 4 s:

| | |
| --- | --- |
| PA6 high (one SCRATCH page erase), 40 erases | min 22.060 ms, mean 22.067 ms, max 22.074 ms |
| period | 99.919 ms (firmware asks 100 ms: HSI16 0.08 % fast against the M2K) |
| firmware's own `erase_us` (report) | 22081 us |

`t_erase` = 22.07 ms, spread 14 us.

## M2 cut to dark (10R in the VBUS_SW wire, bench firmware in measure mode)

`chunky-bench measure --pre-trigger-us 0 --period-ms 100 --cut 5ms --off 1s --captures 5`, M2K scope and logic
free-running at 100 kS/s; the cut is taken from the +5V0 knee. Cuts 2-6 (1 s off before each):

| 3V3 below | after the cut | spread |
| --- | --- | --- |
| 3.0 V | 15.0 ms | 14.9-15.1 |
| 2.5 V | 26.8 ms | 26.4-26.9 |
| 2.0 V | 42.4 ms | 42.2-42.5 |
| 1.7 V | 43.5 ms | 43.4-43.7 |
| 1.0 V | 46.7 ms | 46.5-46.8 |
| 0.3 V | 52.7 ms | 52.5-52.9 |

The regulator drops out at about 42 ms and 3V3 then falls at ~0.3 V/ms, so the G0 goes dark within about a
millisecond of 43.5 ms: `t_dead` ~ 43.5 ms, spread ~0.3 ms. An erase that was running when the cut came (cut
5 ms into it) finishes normally 17.08 ms later (22.08 ms in all): 3V3 is still above 2.9 V then.
+5V0 only falls to ~0.5 V in a 1 s cut, 3V3 reaches 0. The first cut, 300 ms after a power cycle, is ~1 ms
slower (3.0 V at 16.2 ms, 0.3 V at 53.8 ms). An earlier manual cut without the bench firmware running and
without the 10R was slower still (< 0.3 V at 75 ms).

## M3 power off and back

- Off time: 3V3 is below 0.3 V 53 ms after the cut and reaches 0; 300 ms and 1 s both give a clean start.
- Restore: 3V3 above 3.0 V within ~1 ms; PA6's first edge (bootloader, application, first erase,
  `boot_delay_ms = 0`) 1.9 ms after power returns, every time.
- With the 10R series resistor Donguru stays on USB across cut and restore (before: its hub dropped off at
  every restore).

## What follows for the sweep

- The erase is hit when the G0 goes dark inside it: `d` + 43.5 ms in `[pre, pre + 22.1 ms]`. With
  `pre_trigger_us = 50000`: `d` from about 6.5 ms to 28.6 ms; sweep 0-34 ms.
- `off` 300 ms. Control runs at `d` = 200 ms (stock): mark_booted ends `new`, revert ends `old`.
- A run takes 40-75 s: Donguru reads ~12 KB/s over SWD (read-backs and the compare before the restore), and
  the revert path rewrites 34 pages.

## First sweep: `stock-1` (stock bootloader, 12 Oct 2026)

`chunky-bench sweep --name stock-1 --variants stock --paths mark-booted,revert --from 0ms --to 34ms --step 1ms
--fine 0us --pre-trigger-us 50000 --off 300ms --settle 4s`: 70 runs, 52 min, `bench/results/stock-1/runs.csv`.
The two runs at d = 0 are harness errors (the glitcher's delay is at least 3 clock cycles).

| path | d 1-5 ms | d 6-31 ms | d 32-34 ms | bad |
| --- | --- | --- | --- | --- |
| mark_booted (34 runs) | new; cut before the erase: next boot runs the revert tail and B confirms again (2 STATE erases) | new; next boot finds no SWAP magic, B confirms again (1 erase) | new; `mark_booted` had finished | 0 |
| revert (34 runs) | old; cut before the erase: next boot reverts again | old; STATE reads fully erased | old; REVERT magic written | 0 |

No mixed, lost or brick outcome, no ECC flag (FLASH_ECCR 0), no unreadable double word, no PROGERR, in 68 runs.

- The erase is hit for d = 6..31 ms, as predicted from M2 (6.5-28.6 ms) at the lower end. The upper end is
  ~3 ms later than predicted: at d = 29-31 ms the magic written right after the erase is not there. Not
  explained yet (the G0 stops earlier under the erase's load, or the program fails on the falling 3V3); an M2K
  capture at d = 30 ms would show it.
- In every run that hit the erase, STATE read back fully erased, at d = 6 ms already: at 1 ms steps no partly
  erased page was seen. The page goes from untouched (d = 5 ms) to erased (d = 6 ms) within 1 ms, so the tears
  the model in `embassy_torn/REPORT.md` needs (magic kept, progress words partly erased) can only be in that
  1 ms, if they exist on this flash at all.
- "Erased" is what a read at full voltage says; marginal cells are not seen this way.

Next: d = 5.0-6.0 ms in 10-20 us steps with repeats, both paths, then the same on the guarded bootloader.

Harness nit from `stock-1`, fixed: a delay of 0 failed (`--delay is at least 40 ns (3 c)`); it is now raised to
the glitcher's minimum. The late upper edge (erase hit up to d = 31 ms, 3 ms past the prediction) is still open.

## `stock-2-coarse` and `stock-3-fine` (stock, both paths, 12 Oct 2026 evening)

- `stock-2-coarse`: d 5.0-6.0 ms, 100 us steps, 1 repeat, 22 runs, 0 bad. STATE untouched -> fully erased between
  5.7 and 5.8 ms (mark_booted) and between 5.9 and 6.0 ms (revert). No partly erased page.
- `stock-3-fine`: d 5.60-6.10 ms, 10 us steps, 2 repeats, 204 runs in 2 h 38 min, 0 bad, no ECC flag, no
  unreadable word, no PROGERR. **It missed the transition**: in all 204 runs the cut came before the erase had
  any effect (mark_booted: the next boot runs the revert tail and B confirms again; revert: reverted again). The
  edge that sat at 5.7-6.0 ms an hour earlier had moved above 6.1 ms: the G0 went dark at least 0.1-0.4 ms
  sooner after the cut than during the coarse sweep (warm board, supply, or the time between runs; not
  measured). So the edge drifts by more than its own width over an hour, and a fixed fine window is the wrong
  tool.

What this means: 294 stock runs so far, none bad, but only about a dozen of them were within 100 us of the edge
and none is known to have landed inside it. To hit it: locate and sample in one go (refine on what STATE and
the report say, not on the outcome, which is `new`/`old` on both sides of the edge), and repeat that often.

## NRST instead of the power cut: `reset-1` (stock, 13 Oct 2026)

Wiring: glitcher H8 open-drain through 100R on PF2-NRST, B129 powered through the plug. `chunky-bench --cut reset
--width 100us sweep --name reset-1 --variants stock --from 500us --to 24ms --step 250us --fine 50us
--pre-trigger-us 1000 --settle 4s --boots-after 1`: 206 runs (190 coarse, 16 refined), none failed.

| d after PA6 (erase runs 1.00-23.08 ms) | mark_booted (103) | revert (103) |
| --- | --- | --- |
| 0.50-1.05 ms | new; STATE untouched, next boot redoes it | old; STATE untouched, reverted again |
| 1.10-23.15 ms | new; no SWAP magic left, B confirms again | old; STATE reads fully erased |
| 23.20-24.00 ms | new; `mark_booted` had finished | old; REVERT magic written |

0 mixed, 0 lost, 0 brick, FLASH_ECCR 0, no unreadable double word, no PROGERR, in all 206.

- A reset anywhere from 0.1 ms after the erase starts to its end leaves a page that reads fully erased. The page
  goes from untouched to erased between 50 and 100 us after the erase starts; the remaining ~22 ms change nothing
  a normal read can see.
- So the window in which this flash can show the tears the model needs (magic kept, some words erased) is at
  most ~50 us wide, right at the start of the erase. The power-cut sweeps fit that picture (one sharp edge).
- What is read is STATE after the next boot has acted on it, not the torn page itself.

Next: d 1000-1100 us in 2 us steps with repeats, and a raw read of STATE before the bootloader touches it.

## `reset-2-fine`: NRST 1000-1100 us after PA6, stock (13 Oct 2026)

`chunky-bench --cut reset --width 100us --speed-khz 2000 sweep --name reset-2-fine --variants stock --from 1000us
--to 1100us --step 2us --fine 0us --repeats 4 --pre-trigger-us 1000 --boot-delay-ms 3000 --raw-state --settle 8s`:
408 runs in 3 h 33 min (the erase starts 1000 us after PA6).

| d (us) | STATE right after the reset | runs | result |
| --- | --- | --- | --- |
| 1000-1060 | untouched | 248 | all fine (new / old), no ECC flag |
| **1062** | partly erased, or still readable with ECC errors | 8 | **4 brick** (stock bootloader stuck in NMI), 4 fine |
| 1064-1068 | erased, corrected single-bit errors (ECCC) on reading it | 24 | all fine |
| 1070-1100 | erased | 128 | all fine (one more ECCC at 1068-1070) |

The 8 runs at d = 1062 us (reset 62 us into the erase):

| path | STATE magic after the reset | FLASH_ECCR | result |
| --- | --- | --- | --- |
| mark_booted | `fffffffffff7ffff`, 32 double words partly erased | ECCD at 0x08006000 (magic) | **brick:nmi** |
| mark_booted | `fbffffdefcf6ffff` / `fafffffefcf2ffff` / `fafff7fefcf2ffff` (3 runs) | ECCC at 0x08006000 | boots, B confirms again |
| revert | reads SWAP, all 64 progress words still written | ECCD at 0x08006008 (progress validity word) | **brick:nmi** (2 runs) |
| revert | reads SWAP | ECCC at 0x08006008 | reverts again, fine |
| revert | `f0eff1fefcf2f8f1` | ECCC at 0x08006000 on the debugger's read | **brick:nmi** |

- A brick here: core in the NMI handler (IPSR 2), report says the bootloader never returned from
  `prepare_boot`, the application never started. One more reset does not help (same STATE, report again
  without a boot state). ACTIVE and DFU are whole in all four: nothing is lost but nothing boots. This is the
  ECC case of `embassy_torn/REPORT.md`; no mixed or lost image was seen.
- The window is narrow: untouched at 1060 us, erased at 1064 us, so the tear lives in about 2-4 us, 60-64 us
  after the erase starts; 4 of the 8 runs in it brick the stock bootloader.
- Bits go 0 -> 1 across the whole page at once (not word by word): every double word is partly erased in the
  same run. Two reads of the same torn page differ in a few bits (marginal cells).
- Harness gap: the extra boot after a brick was classified from flash content (`new`/`old`) although nothing
  booted (IPSR 0 at the halt, no boot state in the report); the run's final outcome is right (`brick`).

## `reset-2-fine`: NRST into the erase, stock, 13 Oct 2026: first torn results

NRST wiring (bench/README.md "Reset instead of a power cut"), SWD at 2000 kHz (Donguru FW-20).
`chunky-bench --cut reset --width 100us --speed-khz 2000 sweep --name reset-2-fine --variants stock --from 1000us
--to 1100us --step 2us --fine 0us --repeats 4 --pre-trigger-us 1000 --boot-delay-ms 3000 --raw-state --settle 8s
--boots-after 1 --dumps all`: 408 runs in 3 h 34 min, ~31 s per run. `raw_state` is STATE read inside the
bootloader's start delay after the reset, before any code touches it.

| d after PA6 (erase starts at 1000 us) | raw STATE | ECC flag at the raw read | stock outcome |
| --- | --- | --- | --- |
| 1000-1060 us (31 points x 4 x 2 paths) | untouched | none | new / old, all |
| **1062 us** | partly erased, or still reading as before but faulting | ECCD (double) or ECCC (single) | **4 of 8 brick:nmi** |
| 1064-1068 us | reads erased (revert at 1064: 1-5 double words not yet all ones) | ECCC, most runs | new / old, all |
| 1070-1100 us | erased | none | new / old, all |

The 8 runs at d = 1062 us:

- mark_booted r0: every double word partly erased (magic `fffffffffff7ffff`), ECCR `0x80000c00` (ECCD, address
  0x0800_6000 = the magic word): **brick:nmi**, still stuck after the extra reset.
- mark_booted r1-r3: magic garbled (`fbffffdefcf6ffff`, `fafffffefcf2ffff`, `fafff7fefcf2ffff`), ECCC only:
  read as "boot", B confirms again, `new`.
- revert r0, r1: STATE still reads `swap`, all 64 progress words written, but ECCR `0x80000c01` (ECCD at
  0x0800_6008, the progress validity word): **brick:nmi**.
- revert r2: the same picture with ECCC only: reverted again, `old`.
- revert r3: magic `f0eff1fefcf2f8f1`, ECCC at the raw read, ECCD once the bootloader ran: **brick:nmi**.

What it says:

- A torn erase on this G0 is the `Bits` kind of the model, on the whole page at once: every double word drifts
  towards ones together; no word-wise tear was seen. The cells are marginal (reads of the same page differ in a
  few bits), and ECC reports it before the data looks wrong: at 1062 us on the revert path the data still read
  as the old content in 3 of 4 runs, with a double error on one word.
- The window is narrow: one 2 us step (1062 us) is torn, the next three still raise corrected single errors,
  from 1070 us the page is clean. About 62 us after the erase command the cells flip within a few us; the
  remaining ~22 ms of the erase change nothing a normal read can see.
- Stock embassy-boot on G0: the failure is the **ECC brick** (the report's "ECC variant"): the first read of
  STATE takes the NMI and the default handler never returns; a reset does not help, only reflashing STATE.
  4 of 8 runs in the window. No mixed or lost image in 408 runs: the tear that needs (magic kept, validity
  erased, progress partly erased, all readable) did not occur, ECC faults first.
- Power cut vs reset: with a power cut the same window exists (edge at d ~5.8-6.0 ms at pre_trigger 50 ms) but
  drifts by more than its width; 294 power-cut runs did not land in it.

## `reset-3-guarded`: the same window on the guarded bootloader, 13 Oct 2026

`... sweep --name reset-3-guarded --variants guarded --from 1054us --to 1070us --step 1us --fine 0us --repeats 8
--pre-trigger-us 1000 --boot-delay-ms 3000 --raw-state --settle 8s --boots-after 1 --dumps all`: 272 runs in
2 h 24 min. **0 brick, 0 mixed, 0 lost**: mark_booted 136 x `new`, revert 136 x `old`, the application running
after the cut boot and after the extra reset in every run.

Raw STATE right after the reset, 8 runs per delay and path (mark_booted / revert):

| d | data | ECC at the raw read |
| --- | --- | --- |
| 1054-1060 us | untouched (magic already zeroed by `Guarded`) | none |
| 1061 us | untouched | none 6/3, ECCC 2/4, ECCD 0/1 |
| 1062 us | garbled (mark_booted: 2 still untouched) | ECCC 5/5, ECCD 3/3 |
| 1063 us | erased or nearly (11-24 double words not all ones), some garbled | ECCC 6/4, ECCD 2/4 |
| 1064 us | erased | ECCC 5/7, ECCD 3/1 |
| 1065-1069 us | erased | ECCC (clean from 1069 on in some runs) |
| 1070 us | erased | none (one ECCC) |

- 17 runs started the next boot with an uncorrectable ECC error in STATE, the condition that bricked stock in
  every case seen (4 of 4). The guarded bootloader came through all 17: the NMI reader caught the faults
  (`r_ecc_faults` up to 49 in one boot, address 0x0800_6000 or 0x0800_6008: `ADDR_ECC * 8` is right) and
  `Guarded` read the words as zero.
- The tear window is ~3 us wide (1061-1063 us) with a tail of corrected single errors up to ~1069 us: about
  8 us in all, 61 us after the erase starts. It was at the same place 4 hours apart (stock 1062 us).
- Residue: on the revert path the guarded bootloader boots A and leaves the torn page as it is (magic e.g.
  `da5f75debcb2fa7f`, still raising ECC flags on the next boot); it is erased at the next state write. Worth
  deciding whether the bootloader should erase a STATE page it found torn.

## Summary so far (B129A, STM32G0B1CE, one board)

| cut | bootloader | runs | bad |
| --- | --- | --- | --- |
| power (stock-1, -2-coarse, -3-fine) | stock | 294 | 0 (no run known inside the window) |
| NRST, whole erase, 250 us steps (reset-1) | stock | 206 | 0 |
| NRST, 1000-1100 us, 2 us x 4 (reset-2-fine) | stock | 408 | 4 brick:nmi, all at 1062 us (4 of 8 there) |
| NRST, 1054-1070 us, 1 us x 8 (reset-3-guarded) | guarded | 272 | 0 (17 runs with a double ECC error survived) |

The claim of `embassy_torn/REPORT.md` holds on hardware in its ECC form: a reset (or power loss) in a few
microseconds of the STATE erase leaves stock embassy-boot 0.7.0 stuck in the NMI handler for good; the mixed
and lost images of the model's word and bit tears were not seen, because on this flash the page tears as a
whole and ECC faults first. `chunky_swap::Guarded` with the G0 NMI reader survives every tear seen.

## `reset-3-guarded`: NRST 1054-1070 us after PA6, guarded bootloader (13 Oct 2026)

Same command with `--variants guarded --from 1054us --to 1070us --step 1us --repeats 8`: 272 runs in 2.5 h,
**none bad** (mark_booted 136 new, revert 136 old; every boot ran an application).

- Up to 1060 us STATE reads all zero with no ECC flag: `Guarded` has zeroed the magic and the erase has not
  bitten yet. 1061-1063 us: partly erased, 1063-1070 us: erased. The window is the same as on stock.
- The debugger's read of STATE right after the reset saw an uncorrectable error (ECCD) in 17 runs and a
  corrected one (ECCC) in 115. In 13 runs the firmware's own NMI reader caught double errors (up to 49 in one
  boot, at 0x08006000 and 0x08006008) and the boot went on: these are the runs that brick the stock bootloader.
- On the revert path a torn page is left as it is (garbage magic reads as "boot", old image runs); it is only
  erased again by the next `mark_updated`. Its marginal cells stay until then.
- Harness: a boot whose report has no boot state now counts as `brick:stuck` (it was judged from flash content).
