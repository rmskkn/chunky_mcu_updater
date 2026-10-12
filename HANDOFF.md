# Handoff: bench-host (HW-4 power-cut-bench-harness)

Done:
- `embassy_torn::classify`: page classifier as pure fns (Old/New/Mixed/Lost), embassy_torn's tests use it.
- `bench/host` workspace: `chunky_bench` lib (Power+Glitcher, Dut+ProbeDut, restore/read_back, run_once, fold,
  CSV record, resumable sweep with refine, STATE decode, fake bench on real embassy-boot 0.7.0), tests green.

Next: CLI `chunky-bench` (completion, colour, progress, --version), just recipes, README usage, FEATURES/CHANGELOG.

Decisions / gotchas:
- glitch is a git dep (rev 4e55b51) over ssh: `bench/host/.cargo/config.toml` sets git-fetch-with-cli.
- probe-rs without default features (no hidapi): Donguru is CMSIS-DAP v2 only. cargo tree has no -sys but linux-raw-sys (pure Rust).
- fw dir names agreed with parent: boot-{stock,guarded}.bin, app-{a,b}.bin, s0/{state,active,dfu}.bin.
- embassy-boot: an interrupted mark_booted before its erase keeps B (progress invalidated = swap done).
