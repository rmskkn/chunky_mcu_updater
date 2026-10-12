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
| Power switch | Xyloni glitcher `od_out` H8 = J1-15, push-pull (GLT-8, pins GLT-10) | Donguru Q101 gate (Donguru PD1 left as input) |
| Trigger | B129 PA6, J1 pin 18 | Xyloni glitcher trigger in H6 = J1-18 (internal weak pull-down) |
| Marker | Xyloni glitcher marker G8 = J1-14 | M2K DIO2 |
| SWD + CC | Donguru USB-C plug, CMSIS-DAP (typec_dongle 0.3.0, *without* `plug-vbus-on`) | B129 USB-C J3 (SWD on SBU) |
| GND | B129 J1 pin 7 | Xyloni J1-3 / J1-20, M2K (1- and 2- too) |
| M2K A1 (1+) | B129 J2 pin 11 (+3V3) | |
| M2K A2 (2+) | B129 J1 pin 16 (+5V0) | |
| M2K DIO0 | B129 PA6 (J1 pin 18), the capture trigger (rising) | |
| M2K DIO1 | Q101 gate (glitcher H8, Xyloni J1-15) | |
| M2K DIO2 | glitcher marker G8 (Xyloni J1-14) | |
| Status LED | B129 PB14 (blue D14, high = on) | bootloader: on; image A: slow blink; image B: fast blink; measure: on during the erase |

Glitcher idle (`od_out` high) = DUT powered; low = cut. The cut is a fair power loss: Q102 opens and the
DUT's own capacitance (>110 uF on +5V0) decays through the DUT; there is no crowbar to GND.

Donguru drives CC and SWD only while a probe-rs session is attached (`--probe c0de:cafe`, up to 1 MHz) and
floats both when it detaches, so nothing on the plug powers or holds the DUT during a run.

**Back-power hazard (found and fixed, 12 Oct 2026).** Anything that pulls PA6 up while the DUT is off feeds
its protection diode and holds 3V3 up. The first wiring used F7 (weak pull-up in the bitstream) and F6; busgen's
pull resistors soldered on the Xyloni's J1 also coupled F7 through ~7k to the marker on E6, which drove the
powered-off DUT's PA6 during the cut. The glitcher now uses H6 (trigger, internal weak pull-down, no external
resistor), H8 (power out) and G8 (marker): fpga_tools GLT-9, GLT-10. Step M0 below checks it.

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
   trigger H6 rising, delay `d`, `stuck --level low` for `off`. The bootloader waits `boot_delay_ms` first, so
   arming cannot race the boot.
5. The DUT boots on its own, raises PA6 before the STATE erase; the glitcher cuts `d` later.
6. Power returns after `off`; the DUT boots again on its own. Wait, attach, halt, read the report, IPSR,
   FLASH_ECCR and SR, ACTIVE, DFU and STATE.
7. Optionally `boots_after` more plain power cycles, reading back after each (the report's shortest failure
   does its damage on the boot after the cut).
8. Classify (`embassy_torn::classify`, the same page classifier the model's tests use): `old`, `new`, `mixed`,
   `lost`, `brick` (core stuck in NMI or HardFault, or no report). The run's final class: the first brick or
   mixture of any boot, else `lost` if a boot saw an image gone, else what the last boot ran. One CSV row per run.

## Measurement procedure (before any sweep)

What it sets: the erase time `t_erase`, the time from cut to the DUT going dark `t_dead`, the power-off time
`off`, and from them `pre_trigger_us` and the sweep range of `d`. Hold-up estimate to check: 110 uF from 5 V to
the LDO's dropout at ~10 mA is in the order of 10-20 ms, comparable to the ~22 ms page erase, so the cut must be
able to land before PA6 rises: hence `pre_trigger_us`.

Instrument: ADALM2000 in Scopy (M2K CLI has no trigger yet: m2k_cli SCOPE-2, LOGIC-3). Saleae is the fallback
(slower analog). Wiring as in the table: A1 = 3V3, A2 = +5V0, DIO0 = PA6, DIO1 = power out, DIO2 = marker.
Scopy: oscilloscope 1 MS/s or more, 5 ms/div, 1 V/div (5V0 on the same scale), trigger from the logic analyzer
(cross-trigger), logic analyzer trigger DIO0 rising, ~10 % pretrigger. Export each capture as CSV and a
screenshot into `bench/results/measure-<date>/`.

- **M0 static, no firmware needed.** DUT off (`fpga-glitch stuck --level low --width 2s --now`, or Xyloni
  unplugged): 3V3 must fall below 0.1 V and stay there. A plateau is back-power: check the glitcher pins (GLT-10: H6/H8/G8, not F7/F6/E6), the
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

## Firmware (`bench/fw`, HW-3)

Its own cargo workspace (thumbv6m-none-eabi, embassy-stm32 0.6 `stm32g0b1ce`, embassy-boot 0.7.0, chunky_swap
`unsigned`: signing is not what is measured). `just bench-fw` builds all four and writes to `bench/fw/out`:

| File | What | Size |
| --- | --- | --- |
| `boot-stock.bin` / `.elf` | bootloader, STATE plain, default fault handlers | 5.2 KB of 24 KB |
| `boot-guarded.bin` / `.elf` | bootloader, STATE behind `Guarded`, ECC NMI reader | 5.7 KB of 24 KB |
| `app-a.bin` / `.elf` | application, image A, padded to 32 KB with the filler | 5.0 KB used |
| `app-b.bin` / `.elf` | application, image B, padded likewise | 5.2 KB used |

The `.bin` files start at their region (BOOTLOADER, ACTIVE); `bench-fw-image` (`bench/fw_image`) cuts them from
the ELF and pads the apps. Crates: `bench_fw_common` (flash regions through the ECC reader, `TriggerFlash`, the
report, TIM2 as a 1 MHz counter), `bench_bootloader` (features `stock` | `guarded`), `bench_app` (features
`image-a` | `image-b`). memory.x comes from `bench_layout` in each `build.rs`, RAM ending before REPORT.

- **Bootloader**: PA6 push-pull low, the report reset (variant), the config read, `boot_delay_ms`, then
  `prepare_boot` and its result into `boot_state`, then VTOR and jump to ACTIVE. On a `prepare_boot` error it
  stays in thread mode with `boot_state = Error` (no reset, so the failure stays visible).
- **Application**: `app_image`, `app_stage = Running`. Run: B confirms (`Confirmed` or `Error`) or waits
  `reset_after_ms`, sets `Resetting` and resets; A runs. Measure: one SCRATCH erase behind the trigger every
  `period_ms`, `erase_us` each time. Provision: `mark_updated`, then `bkpt` (with no probe attached that is a
  HardFault). STATE goes through the wrapper of the bootloader that ran (the report's `variant`): `Guarded` after
  `guarded`, plain after `stock`; after `stock` an ECC NMI hangs in the app's handler like the default one.
- **Trigger**: PA6 goes high only for STATE (and SCRATCH) erases, nowhere else: there is no marker pulse at
  boot, as it would fire the armed glitcher. For M3, take the bootloader's start from the 3V3 rise.
- **ECC reader** (guarded, and the app after guarded): the NMI checks FLASH_ECCR and ECCR2, clears ECCD, adds one
  to `ecc_faults` and puts the address into `ecc_last_addr` (`0x0800_0000 + ADDR_ECC * 8`: ADDR_ECC read as a
  double-word offset, check this at bring-up against a known torn word). An NMI without ECCD hangs. The read that
  ran into it returns `Ecc`, which `Guarded` masks as zeros (STATE) and which fails `prepare_boot` for ACTIVE and
  DFU reads.

## The harness: `chunky-bench` (HW-4)

`bench/host` is its own cargo workspace (probe-rs and the glitcher stay out of the main one): the library
`chunky_bench` (`Power`: the glitcher, `Dut`: probe-rs, the run loop, sweeps, CSV, a simulated bench) and the
thin CLI `chunky-bench`. Run it from the repository root, so its defaults (`--fw bench/fw/out`,
`--results bench/results`) fit; `--help` on every command says the rest.

```sh
just bench-test                       # harness tests, on the simulated bench (no hardware)
just bench-lint                       # clippy + fmt (also part of `just lint`)
CARGO_NET_GIT_FETCH_WITH_CLI=true cargo install --path bench/host/chunky_bench_cli --locked  # glitch is a git dep over ssh
echo 'source <(COMPLETE=bash chunky-bench)' >> ~/.bashrc    # completion: probes, glitcher ports, sweep names (zsh, fish alike)
```

Every command takes `--probe` (default `c0de:cafe`), `--chip` (`STM32G0B1CEUx`), `--speed-khz` (1000),
`--glitcher` (default: the first Xyloni), `--fw`, `--results`, and `--dry-run`: the simulated bench and stand-in
firmware instead of the hardware (results then go to `bench/host/target/dry-run`).

| Command | What |
| --- | --- |
| `status` | attach, halt, read back once and classify; no power action |
| `cycle --off 300ms` | one plain power cycle |
| `snapshot` | provisioning: stock bootloader, A in ACTIVE, B in DFU, STATE erased, `Mode::Provision`; A runs `mark_updated`; saves S0 to `<fw>/s0/{state,active,dfu}.bin` (checks: SWAP magic, no progress, A and B in place) |
| `measure` | M1-M3: writes the bootloader and a `Mode::Measure` config (`--pre-trigger-us`, `--period-ms`), power-cycles; without `--cut` reads the firmware's `erase_us` after `--wait`, with `--cut D` fires `--captures` cuts D after PA6, `--off` long (`just bench-measure ...`) |
| `run --variant V --path P --delay D --pre-trigger-us N` | one run, every boot printed |
| `sweep --name S --from D0 --to D1 --pre-trigger-us N` | a sweep (`just bench-sweep ...`) |

Firmware directory (`bench/fw/out`, built by HW-3): `boot-stock.bin`, `boot-guarded.bin`, `app-a.bin`,
`app-b.bin` (padded to 32 KB with `bench_layout::pad_image` when shorter), and `s0/` from `snapshot`. A run
restores the bootloader of its variant, the config and S0, writing only the pages that differ, and verifies them.

Run parameters (`run`, `sweep`): `--pre-trigger-us` (required, from the measurement), `--off` (300ms),
`--boot-delay-ms` (100: the window in which the harness arms the glitcher), `--reset-after-ms` (100),
`--settle` (5s: from arming to the read-back; swap, cut, power back and reboot must fit), `--boots-after` (1).

**Sweep.** `--variants stock,guarded` × `--paths revert,mark-booted` × delays `--from..=--to` by `--step` (100us)
× `--repeats` (1). After the coarse grid, wherever two neighbouring delays of a variant and path have different
sets of outcomes, the points between them run at `--fine` (10us; 0 turns it off). Everything goes to
`bench/results/<name>/runs.csv`, one row per run, flushed as it goes; the CSV is the sweep's only state, so the
same command resumes it (points with a row are skipped, `error` rows run again). `--max-errors` (3) failed runs
in a row stop it. Raw read-backs per run (`--dumps all|bad|none`) in `dumps/<variant>-<path>-d<ns>-r<repeat>/`:
`boot<N>-{report,state,active,dfu}.bin`, unreadable double words as 0xEE. A progress bar with ETA, then a table
of outcomes per variant and path.

CSV columns: `time`, `sweep`, `variant`, `path`, `d_ns`, `repeat`, `pre_trigger_us`, `off_us`, `settle_ms`,
`boots_after`, `fired` (the glitcher's cut fired), `final` (old/new/mixed/lost/brick/error), `outcome` (with the
page map of a mixture, e.g. `mixed[A0 B1 ..]`, or `brick:nmi`). Per boot, `|`-separated (the boot after the cut
first): `boots`, `flash` (the flash class even when bricked), `ipsr`, `eccr` (as found, before the harness read
flash), `eccr_read` (after its reads: ECC errors the debugger hit), `sr`, `progerr`, `state_magic`
(boot/swap/revert/dfu/erased/zero/ecc/raw), `state_progress` (embassy-boot's index, `!` when the validity word is
written), `state_written`, `unreadable`. The report of the boot after the cut: `r_variant`, `r_boot_state`,
`r_app_image`, `r_app_stage`, `r_ecc_faults`, `r_ecc_last_addr`, `r_erase_us`, `r_state_erases`. Then
`restored` (pages written), `dump`, `error`.

ECCR and SR are recorded raw (and ECCR's ECCD/ECCC cleared after each read-back); the ECC address field is not
decoded yet.

## Gotchas

- G0 flash has ECC (always on). A torn erase or program can leave double-word ECC errors: the CPU gets an NMI on
  reading them (stock: hangs in the default handler, the report says nothing past the bootloader); a debugger read
  sets FLASH_ECCR.ECCD. The harness reads and clears ECCR after each read-back and records the address.
- Writing all zeros over a programmed double word is allowed on G0 (embassy-boot's revert does it); the harness
  records FLASH_SR so any PROGERR shows.
- The STATE erase stalls code fetch from bank 1 while it runs: the trigger GPIO must be set before the erase, and
  the firmware cannot do anything during it.
- Never flash or power-cycle the bench without telling whoever is at it first.
