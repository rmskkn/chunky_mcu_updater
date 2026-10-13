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
