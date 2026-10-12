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
