# Handoff: power-cut-bench (P2625#11oct-1213)

## Done
- Plan; contract crate `bench/bench_layout` (layout, Config in flash, Report in RAM, filler; tests) and
  `bench/README.md` (wiring, layout, paths, run order, M2K measurement procedure M0-M3, gotchas). Commit d043ae2.
- Ids: HW-3 (fw), HW-4 (harness), GLT-9 (fpga_tools trigger pull); ECC-2's G0 half rides on HW-3.
- Tasks and sessions:
  - P2625#12oct-1432 HW-3 bench fw: session bench-fw (opus, omarchy-m1, from d043ae2)
  - P2625#12oct-1432b HW-4 harness: session bench-host (opus, omarchy-m1, from d043ae2); told to use a git dep
    on fpga_tools rev 4e55b51 for the glitch lib
  - P2625#12oct-1432c GLT-9 trig pull: session glt-trig-pull (sonnet, gpd-omarchy, Efinity is there)
  - Later: #12oct-1433 SIM tear model from hw, #12oct-1433b bench extras (BOR, per-run M2K, 2-page STATE),
    #12oct-1433c BOOT-5 results to REPORT.md + upstream

## Child status
- glt-trig-pull (gpd-omarchy): ready, GLT-9 done (TRIG_PULL build option, default NONE; 1482 LEs, 81.1 MHz), not
  landed, not loaded. The bitstream is on gpd-omarchy; Efinity (and its programmer) is x86 only, so the Xyloni
  is loaded from gpd-omarchy or with another loader here: ask the user which.
- bench-fw: ready, merged here (614f160); `just bench-fw` builds boot-stock 5.2 KB, boot-guarded 5.7 KB, apps
  ~5 KB padded to 32 KB into bench/fw/out. Not run on hardware. Its notes: no PA6 pulse at boot (would fire the
  armed glitcher), so M3 takes bootloader start from the 3V3 rise; prepare_boot error = hang in thread mode with
  boot_state=Error; Provision's bkpt needs the probe attached; ecc_last_addr = base + ADDR_ECC*8, check once on
  hardware.
- bench-host: ready, merged here (conflicts in README/FEATURES/CHANGELOG/justfile resolved, both kept).
  `just lint`, `just test`, `just bench-test` green on the merged branch; `chunky-bench --dry-run sweep` writes a
  CSV. Hardware untested. Commands: status, cycle, snapshot, measure, run, sweep (bench/README.md).

## Bench state, 12 Oct 2026 evening
- GO received. Glitcher GLT-10 flashed: trigger H6 (J1-18, weak pull-down), power out H8 (J1-15), marker G8
  (J1-14). M2K: A1 3V3, A2 5V0, DIO0 PA6, DIO1 power out, DIO2 marker. Tools: ~/git/fpga_tools/host/target/release/
  fpga-glitch, ~/git/m2k_cli/target/release/m2k, bench/host/target/release/chunky-bench (built).
- Manual cut by the parent (default BOR): 3V3 > 3.0 V for ~20 ms, 2.79 V @30 ms, 2.32 @50, 2.11 @60, < 0.3 V
  @75 ms; 5V0 1.5 V @100 ms. The G0 runs 60-70 ms past the cut: pre_trigger_us must be ~80 ms or more, `off`
  well over 100 ms (5V0 is slow; 2 s cut recovered cleanly).
- LED on PB14 added (6f28f97), README wiring updated. Binaries rebuilt in bench/fw/out.
- BLOCKED: Donguru dropped off USB when H8 switched the DUT on (suspected Q102 inrush browning out its hub).
  Parent said hold the hardware; it resets Donguru via J-Link and tests a cut/restore cycle. If power-restore
  drops the probe every time, the bench needs an inrush fix before any sweep.

## Next (this session, with the user at the bench)
1. When the parent says the Donguru is back: `chunky-bench status`, then M1 (`just bench-measure`), M2
   (`--pre-trigger-us 90000 --cut 0`, M2K capture), M3; numbers to `bench/results/measure-2026-10-12/NOTES.md`
   and `tpm work say parent`. Default BOR, no option-byte changes.
2. `chunky-bench snapshot`, then first sweeps: stock vs guarded, revert and mark_booted.

## Decisions
- One-page STATE, everything in bank 1; images 32 KB with per-page filler; signing off (unsigned) on the bench.
- Trigger inside the STATE wrapper on the raw erase, with `pre_trigger_us` before it, so the cut can land before
  the erase despite hold-up (estimate 10-20 ms at ~10 mA vs ~22 ms erase).
- `boot_delay_ms` in the bootloader so arming the glitcher after the power cycle never races the boot.
- Stock = embassy-boot as users get it (default NMI handler: an ECC tear hangs = "brick"); guarded = Guarded +
  G0 ECC NMI reader.

## Gotchas
- Glitcher F7 has WEAK_PULLUP: back-powers the DUT via PA6 when off (GLT-9 + 100k pull-down).
- Xyloni was not on USB at planning time (no FT4232H in lsusb); Donguru c0de:cafe and a J-Link are.
- Efinity is on gpd-omarchy only (x86); this PC is aarch64.
