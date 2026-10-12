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

## M2 cut to dark (one manual cut so far, M2K 10 kS/s, firmware idle)

| after the cut | 3V3 |
| --- | --- |
| 0-20 ms | > 3.0 V |
| 30 ms | 2.79 V |
| 50 ms | 2.32 V |
| 60 ms | 2.11 V |
| ~65 ms | regulator drops out |
| 75 ms | < 0.3 V |

+5V0 falls slowly (1.5 V at 100 ms). A 2 s cut recovers cleanly (3.0 V at its end). The G0 runs 60-70 ms past
the cut at the default brown-out level. To do with the measure firmware and the trigger: `t_dead` with its
spread over several cuts, during an erase and idle.

## M3 power off and back

Open. Known: power restore through Q102 dips Donguru's supply and its USB hub drops off the bus (inrush into
the B129's >110 uF); cut runs wait for that fix.

## What follows for the sweep (provisional)

- `pre_trigger_us` about 80000-90000 (>= `t_dead`(max) + 2 ms), to be fixed once M2 has the spread.
- `off` at least 300 ms for 3V3; +5V0 is still at 1.5 V after 100 ms, so check that a short `off` gives a real
  power-on reset (M3) or use 1-2 s.
- Sweep `d` from `pre - t_dead(max) - 1 ms` to `pre + 22.1 ms - t_dead(min) + 1 ms`.
- With 60-70 ms of slow decay the instant the erase stops is set by where the flash gives up on the falling
  3V3, not by the glitcher's 13 ns steps: expect ms of jitter per delay, so repeats per point matter more than
  a fine step. Brown-out level 4 would tighten it (later option).
