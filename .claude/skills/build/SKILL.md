---
name: build
description: Build, run, and test the chunky_bootloader PoC (poc/). Use whenever compiling, running cb-server/cb-client/cb-flash, running the test suite, or checking the build across geometries.
---

# chunky_bootloader PoC: build / run / test

All commands run from `poc/` unless noted. Binaries land in `poc/build/`.

## Build

```sh
make                      # build cb-server, cb-client, cb-flash (incremental)
make clean                # remove build/ and tests/run/
make clean all            # full rebuild
```

Geometry is a build-time choice, baked in via `-D`:

```sh
make ERASE_SIZE=1024 SECTOR_SIZE=2048   # rebuild at a different geometry
make DEVICE=stm32f103cb                 # device catalog entry: device/<DEVICE>/device.toml
                                        # -> build/device/geometry.h + memory.ld (needs Python 3.11+)
./build/cb-flash geometry               # print the geometry a given build was compiled with
```

## Run (manual two-process demo)

```sh
scripts/lab.sh server &        # start the server, keep it running
scripts/lab.sh release [RUNS]  # synthesize/publish a release (default 4 patched sectors)
scripts/lab.sh release FILE    # publish your own firmware binary as the release
scripts/lab.sh update          # device asks for the update: manifest -> delta -> apply -> verify
scripts/lab.sh status          # where the device stands
scripts/lab.sh manifest        # inspect the device's current flash
scripts/lab.sh map             # print the flash layout
scripts/lab.sh reset           # wipe the simulated device, start fresh
```

Scripted end-to-end walkthrough (own server/client pair, auto-cleaned):

```sh
scripts/run_demo.sh
```

### Binaries directly

```sh
./build/cb-server --port PORT --image RELEASE.bin --cache CACHE_DIR
./build/cb-client --flash FLASH.bin --initial BASE.bin --port PORT
./build/cb-flash manifest FLASH.bin      # per-sector hashes + digest
./build/cb-flash map                     # this build's flash layout (from geometry.h, no file)
./build/cb-flash geometry                # this build's geometry (shell-sourceable)
./build/cb-flash mutate IN OUT RUNS SEED # synthesize a new release
./build/cb-flash find FLASH.bin HASH     # which sectors hold this hash
```

## Test

```sh
bash tests/run_all.sh        # full suite: compress_test + every case_*.sh
make test-compress           # just the LZ4 decoder under ASan/UBSan
bash tests/case_noop_rerun.sh # a single case, run directly
```

Run the whole suite across every supported geometry (clean rebuild per entry, restores the default build afterward):

```sh
bash tests/run_matrix.sh
```

## Notes

- `make` has no header-dependency bug to work around: object rules depend on every included header, the `Makefile` itself, and a geometry stamp — a plain `make` after editing any `.h` or the `Makefile` recompiles exactly what's needed.
- Object dirs mirror the source layers: `build/obj/` (lib), `build/obj/plat/` (platform), `build/obj/mw/` (middleware), `build/obj/bin/` (src). lib/ is compiled with only `lib/` and `build/device/` on its include path, so a platform/middleware include there fails the build.
- Tests each get their own scratch dir under `tests/run/<name>` and their own port — safe to run concurrently.
