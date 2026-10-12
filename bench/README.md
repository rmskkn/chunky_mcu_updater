# Power-cut bench: B129A, embassy-boot's STATE erase on real flash

Does a power cut during embassy-boot's STATE erase lose images on a real STM32G0B1, as the model in
[`embassy_torn/REPORT.md`](../embassy_torn/REPORT.md) says, and does `chunky_swap::Guarded` hold there too?
FEATURES.md: HW-3 power-cut-bench-fw, HW-4 power-cut-bench-harness, ECC-2 ecc-fault-handlers (the G0 half).
tpm: P2625#11oct-1213.

- `bench_layout/` — the contract: flash and RAM layout, the config record (flash) and report record (RAM).
- `fw/` — the B129A test firmware (own workspace, thumbv6m): bootloader `stock` and `guarded`, application
  images A and B, the PA6 trigger. (HW-3)
- `host/` — the harness `chunky-bench` (own workspace): fpga-glitch + probe-rs, sweeps, CSV. (HW-4)
- `results/` — CSV and raw dumps of the runs, the measurement notes.

## Wiring

| What | From | To |
| --- | --- | --- |
| DUT power | Donguru Q102 drain, header J101 pin 10 (VBUS_SW) | B129 J1 pin 16 (+5V0) |
| Power switch | Xyloni glitcher `od_out` J1 F6, push-pull (GLT-8) | Donguru Q101 gate (Donguru PD1 left as input) |
| Trigger | B129 PA6, J1 pin 18 | Xyloni glitcher trigger in J1 F7 |
| SWD + CC | Donguru USB-C plug, CMSIS-DAP (typec_dongle 0.3.0, *without* `plug-vbus-on`) | B129 USB-C J3 (SWD on SBU) |
| GND | B129 J1 pin 7 | Xyloni, M2K |
| M2K 1+ | B129 J2 pin 11 (+3V3) | |
| M2K 2+ | B129 J1 pin 16 (+5V0) | |
| M2K DIO0 | B129 PA6 (J1 pin 18) | |
| M2K DIO1 | Q101 gate (glitcher F6) | |

Glitcher idle (`od_out` high) = DUT powered; low = cut. The cut is a fair power loss: Q102 opens and the
DUT's own capacitance (>110 uF on +5V0) decays through the DUT; there is no crowbar to GND.

Donguru drives CC and SWD only while a probe-rs session is attached (`--probe c0de:cafe`, up to 1 MHz) and
floats both when it detaches, so nothing on the plug powers or holds the DUT during a run.

**Back-power hazard.** The glitcher bitstream gives the trigger input F7 a weak pull-up. With the DUT off, that
pull-up feeds PA6's protection diode and can hold the DUT's 3V3 up (no clean power-on reset, a slower or
partial power loss). Fix: the bitstream with F7 pull `NONE` (fpga_tools GLT-9) plus a 100k pull-down from F7
to GND. Step M0 below checks it either way.

## Memory layout (STM32G0B1CE, bank 1, 2 KB pages)

| Region | Address | Size | |
| --- | --- | --- | --- |
| BOOTLOADER | 0x0800_0000 | 24 KB | `stock` or `guarded` build |
| STATE | 0x0800_6000 | 2 KB | one page: one erase is swept |
| CONFIG | 0x0800_6800 | 2 KB | `bench_layout::Config`, written by the harness |
| SCRATCH | 0x0800_7000 | 2 KB | erased by the measure mode |
| ACTIVE | 0x0800_8000 | 32 KB | 16 pages; app linked here |
| DFU | 0x0801_0000 | 34 KB | ACTIVE + 1 page |
| REPORT (RAM) | 0x2002_3F00 | 256 B | `bench_layout::Report`, not initialised by the runtime |

Images A and B are the same application with a different id, padded to 32 KB with
`bench_layout::filler_word`, so every page of A differs from the same page of B and a page-wise mix shows.

## The two paths, the two bootloaders

The only difference between the bootloader builds is the STATE partition: `stock` hands embassy-boot 0.7.0 the
plain flash partition (default fault handlers, as an embassy-boot user gets it), `guarded` wraps it in
`chunky_swap::Guarded` (magic zeroed before every erase, ECC faults masked through the G0 NMI reader). The
application uses the same wrapper for `mark_booted`. The trigger sits *inside* the wrapper, on the raw erase,
so both builds are cut against the same erase: PA6 goes high, `pre_trigger_us` busy wait, page erase, PA6 low.

- **mark_booted path** (`confirm = true`): from snapshot S0 (A in ACTIVE, B in DFU, magic SWAP, as
  `mark_updated` leaves it) the bootloader swaps, B runs and calls `mark_booted`; its STATE erase is cut.
- **revert path** (`confirm = false`): the same start, B runs unconfirmed and resets after `reset_after_ms`;
  the bootloader's revert erases STATE; that erase is cut.

## One run

1. Power on (glitcher idle), probe-rs attach.
2. Restore S0, the config and both images (only pages that differ are written).
3. Detach (Donguru floats SWD and CC).
4. Power-cycle with the glitcher (software trigger, `off` long enough for a clean power-on reset), then arm it:
   trigger F7 rising, delay `d`, `stuck --level low` for `off`. The bootloader waits `boot_delay_ms` first, so
   arming cannot race the boot.
5. The DUT boots on its own, raises PA6 before the STATE erase; the glitcher cuts `d` later.
6. Power returns after `off`; the DUT boots again on its own. Wait, attach, halt, read the report, IPSR,
   FLASH_ECCR and SR, ACTIVE, DFU and STATE.
7. Optionally `boots_after` more plain power cycles, reading back after each (the report's shortest failure
   does its damage on the boot after the cut).
8. Classify (embassy_torn's classifier on pages): `old`, `new`, `mixed`, `lost`, `brick` (core stuck in NMI or
   HardFault, or no report). One CSV row per run.

## Measurement procedure (before any sweep)

What it sets: the erase time `t_erase`, the time from cut to the DUT going dark `t_dead`, the power-off time
`off`, and from them `pre_trigger_us` and the sweep range of `d`. Hold-up estimate to check: 110 uF from 5 V to
the LDO's dropout at ~10 mA is in the order of 10-20 ms, comparable to the ~22 ms page erase, so the cut must be
able to land before PA6 rises: hence `pre_trigger_us`.

Instrument: ADALM2000 in Scopy (M2K CLI has no trigger yet: m2k_cli SCOPE-2, LOGIC-3). Saleae is the fallback
(slower analog). Wiring as in the table: 1+ = 3V3, 2+ = +5V0, DIO0 = PA6, DIO1 = glitch out.
Scopy: oscilloscope 1 MS/s or more, 5 ms/div, 1 V/div (5V0 on the same scale), trigger from the logic analyzer
(cross-trigger), logic analyzer trigger DIO0 rising, ~10 % pretrigger. Export each capture as CSV and a
screenshot into `bench/results/measure-<date>/`.

- **M0 static, no firmware needed.** DUT off (`fpga-glitch stuck --level low --width 2s --now`, or Xyloni
  unplugged): 3V3 must fall below 0.1 V and stay there. A plateau is back-power: check F7's pull-up (GLT-9), the
  M2K DIO pins, Donguru SWD/CC (no probe-rs session). Then glitcher idle: +5V0 ~5 V, 3V3 3.3 V.
- **M1 erase time.** Flash measure mode (`Mode::Measure`, `pre_trigger_us = 0`, `period_ms = 100`): PA6 is high
  for exactly one SCRATCH erase. Over 20+ erases note min / typical / max of the PA6 high time = `t_erase`. The
  app also stores the last erase time in the report (`erase_us`) as a cross-check.
- **M2 cut to dark.** Measure mode with `pre_trigger_us = 30000`, glitcher `stuck --level low --width 500ms
  --ext rise --delay 0` (re-armed for each capture). From DIO1 falling: time for 3V3 to cross 3.0 V, 2.0 V and
  1.7 V (G0 power-down reset), and the 5V0 curve. `t_dead` = cut to 3V3 < 1.7 V; repeat 5 times. Note whether
  PA6 stays high until the MCU dies. Repeat with `pre_trigger_us = 0` and delay 5 ms (cut during an erase: the
  erase draws more current).
- **M3 power off and back.** Time from the cut until 3V3 < 0.3 V (`off` must exceed it with margin, for a clean
  POR); 3V3 rise time and the delay until the bootloader drives PA6 (a short PA6 pulse at boot, if the firmware
  marks it) after power returns.

From these: `pre_trigger_us >= t_dead(max) + 2 ms`; sweep `d` from `pre_trigger_us - t_dead(max) - 1 ms` to
`pre_trigger_us + t_erase(max) - t_dead(min) + 1 ms`, 100 us coarse, then 10 us around every change of outcome.
Write the numbers into `bench/results/measure-<date>/NOTES.md`; the harness reads none of it, the sweep command
takes them as arguments.

## Gotchas

- G0 flash has ECC (always on). A torn erase or program can leave double-word ECC errors: the CPU gets an NMI on
  reading them (stock: hangs in the default handler, the report says nothing past the bootloader); a debugger read
  sets FLASH_ECCR.ECCD. The harness reads and clears ECCR after each read-back and records the address.
- Writing all zeros over a programmed double word is allowed on G0 (embassy-boot's revert does it); the harness
  records FLASH_SR so any PROGERR shows.
- The STATE erase stalls code fetch from bank 1 while it runs: the trigger GPIO must be set before the erase, and
  the firmware cannot do anything during it.
- Never flash or power-cycle the bench without telling whoever is at it first.
