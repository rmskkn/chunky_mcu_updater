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
- M1 done (no cuts): page erase 22.060 / 22.067 / 22.074 ms (min/mean/max, 40 erases), firmware erase_us
  22081. B129 now holds stock bootloader + image A + measure config (flashed by hand with probe-rs; config bytes
  in the job tmp, the harness's `measure` writes the same now, plus image A and a blank STATE). Notes:
  bench/results/measure-2026-10-12/NOTES.md. Reported to the parent.
- Inrush fixed by a 10R series resistor in the VBUS_SW wire (Donguru stays on USB; its FW-18 also recovers the
  hub). M2: G0 dark ~43.5 ms after the cut (spread 0.3 ms); M3: 300 ms off is a clean POR, first PA6 edge 1.9 ms
  after restore. S0 snapshot in bench/fw/out/s0 (untracked build output: take it again after any fw rebuild).
- Sweep stock-1 done (bench/results/stock-1): 68 runs, 0 bad. Erase hit for d 6..31 ms at pre_trigger 50 ms;
  STATE always reads fully erased when hit; transition untouched -> erased between d 5 and 6 ms.

- stock-2-coarse (22 runs) and stock-3-fine (204 runs): none bad; the fine window 5.60-6.10 ms missed the edge,
  which had drifted above 6.1 ms within the hour. 294 stock power-cut runs in all, none known inside the edge.
- Software since: `--cut reset --width` (NRST pulse, open-drain), d clamped to the glitcher minimum, sweep
  refinement keyed on STATE magic / boot state / STATE erases of the boot after the cut. Release binary rebuilt
  (bench/host/target/release/chunky-bench). All dry-run only.

- 13 Oct: NRST wiring (H8 open-drain -> 100R -> B129 J1-6, B129 powered through the plug, Donguru 0.4.1
  plug-vbus-on). reset-1 (stock, 206 runs): none bad; STATE flips untouched -> fully erased 50-100 us after the
  erase starts (d 1.05 -> 1.10 ms at pre_trigger 1 ms) and reads erased for the rest of the erase.
- Harness since: SWD reset-and-run before detaching on reset wiring, `--raw-state` (STATE as the cut left it,
  needs --boot-delay-ms 3000). Release binary rebuilt. Dry-run tested only.

## Next
1. HOLD: session dap-512 flashes Donguru (FW-19, 512-byte DAP packets) and tests on this bench. No hardware
   run until the parent says the bench is free. Then re-check once: `chunky-bench --cut reset status`, one
   control run per path (restore works, cut fires).
2. Fine sweep (approved): `--cut reset --width 100us sweep --name reset-2-fine --variants stock --from 1000us
   --to 1100us --step 2us --fine 0us --repeats 2 --pre-trigger-us 1000 --boot-delay-ms 3000 --raw-state
   --settle 8s --boots-after 1 --dumps all`. Report bad / partly erased / ECC rows at once. Then guarded (say
   before starting).
3. Open: late upper edge of the power-cut window; glitch git rev still 4e55b51.
4. Before `tpm work ready`: `git rm HANDOFF.md`, `just lint`, `just test`.

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
