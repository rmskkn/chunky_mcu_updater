# Host PoC

A working implementation of the `DESIGN.md` update path, built with gcc and run
entirely on the host. Three binaries talk the real protocol over a real TCP
socket; the device side owns a 128 KB file that is treated as STM32F103CB flash,
with page erase, half-word programming, and the rule that a program may only
clear bits.

The firmware under test is a real binary: `klipper.bin` (45 772 B) is split into
sectors, wrapped in an image descriptor, and transferred sector by sector.

## Build and run

```sh
make                       # -> build/cb-{server,client,flash}
scripts/run_demo.sh        # provision v1, update to v2, apply, confirm
scripts/run_resume.sh      # power loss mid-download, then resume
scripts/run_unroll.sh      # tampered image, and a trial boot that never confirms
scripts/overhead.sh        # measured wire cost against a full transfer
scripts/timing.sh          # time per attempt, host and modelled device time
scripts/lab.sh setup       # then drive server and client by hand, see below
```

Everything is written under `run/`, which is disposable. The reference binary
comes from `$KLIPPER_BIN` (default `/home/roman/projects/klipper/out/klipper.bin`).

## Running it by hand

The scripts above start and stop a server for you. For interactive work use
`scripts/lab.sh`, which keeps one server up while you drive the device:

```sh
scripts/lab.sh setup            # fresh flash, slot 0 provisioned with v1
```

Terminal 1, left running:

```sh
scripts/lab.sh server           # add --no-copy, --full or --tamper N here
```

Terminal 2:

```sh
scripts/lab.sh release 2        # publish v2 (4 patched runs)
scripts/lab.sh session          # run one update session
scripts/lab.sh boot             # the bootloader applies and verifies
scripts/lab.sh confirm          # pass the self-test
scripts/lab.sh status           # where everything stands
```

The server stays up across sessions and re-reads the image directory each
time, so `release 3 12` then `session` tests a new release without restarting
it. Anything `lab.sh` does not recognise is passed to `cb-client`, so
`scripts/lab.sh session --fail-after 20` cuts the power mid-download and
`scripts/lab.sh unroll` abandons a staged plan.

Without the harness it is two plain commands:

```sh
build/cb-server --port 7700 --images run/images --cache run/cache
build/cb-client session --flash run/flash.bin --port 7700
```

Ctrl-C stops the server, and it does not outlive the shell that started it:
`lab.sh` execs it so signals reach it directly, and it sets
`PR_SET_PDEATHSIG` so a closed terminal or a killed wrapper script cannot leave
it holding the port. If a bind ever fails anyway, the error says what to do:

```
error: cannot listen on port 7700: Address already in use
  another server is probably still running: pkill cb-server
  or pick a free port on both sides: --port N
```

A vanished *device*, on the other hand, is an ordinary outcome: the server
ignores `SIGPIPE`, so a client killed mid-transfer does not take it down:

```
server: device went away after 5 steps
server: steps skip=7 copy=16 write=29
...
server: steps skip=12 copy=16 write=24      # next session resumes the gap
```

## What the pieces are

| Binary | Role |
|---|---|
| `cb-flash` | Creates and inspects the emulated flash; provisions a slot; builds and mutates test releases |
| `cb-server` | Holds releases, verifies the device manifest, diffs, streams only differing sectors |
| `cb-client` | The device. `session` runs an update; `boot` is the bootloader; `confirm`, `unroll`, `status` |

| Source | Covers |
|---|---|
| `flash.c` | Page erase, half-word program with the bits-only-clear rule, meta bank ping-pong, journal |
| `manifest.c` | Canonical manifest body, digest, MAC, structural checks |
| `plan.c` | The three planner rules of `DESIGN.md` section 5 |
| `image.c` | Image descriptor at slot offset `0x200`, `image_sha256` domain with the descriptor hole |
| `frame.c` | `A5 5A | len | type | seq | payload | crc32` framing and the byte counters |
| `client.c` | Session, then the bootloader: apply, verify, unroll, trial boot, revert |

## Flash map

`cb-flash map` prints it. It is the layout from `DESIGN.md` section 2, with the
device identity page from 10.2:

```
bootloader          0-14     0x00000     15360
device identity    15        0x03c00      1024
meta bank 0        16-19     0x04000      4096
meta bank 1        20-23     0x05000      4096
slot 0             24-75     0x06000     53248
slot 1             76-127    0x13000     53248
```

## Measured overhead

From `scripts/overhead.sh`. Baseline is the same protocol with every sector
forced to a write: 60 724 B.

```
case                          skip  copy  write      wire of full
1 patched run                   50     0      2      4574    7.5%
4 patched runs                  47     0      5      7943   13.1%
12 patched runs                 40     0     12     15804   26.0%
40 patched runs                 26     0     26     31526   51.9%
4 runs, rule 2 off              47     0      5      7943   13.1%
16 B inserted (shift)           10     0     42     49494   81.5%
full image, forced               0     0     52     60724  100.0%

first update, erased             7    40      5      9583   15.8%
first update, no copy            7     0     45     52863   87.1%

1 run, warm cache               50     0      2      2498    4.1%
4 runs, warm cache               47    0      5      5867    9.7%
```

Three results are worth reading twice.

**Rule 2 (local copy) is worth nothing in steady state and a great deal on a
first update.** With the destination slot holding the previous release, turning
it off changes the session not at all. Updating into an *erased* slot, it finds
40 of 45 sectors already on the device and takes the session from 87.1 % to
15.8 %.

**The layout hazard is real and large.** Inserting 16 bytes near the start of
the binary shifts everything after it: the same logical change goes from 5
changed sectors to 42, and from 13.1 % to 81.5 %. This is the argument for
reproducible builds and a stable link order in `DESIGN.md` section 17.

**Manifest elision pays on exactly the updates that need it.** A warm cache is
worth a flat 2 076 B, which is more than a third of a two-sector session.

## Measured time per attempt

From `scripts/timing.sh`. Host wall time is reported for what it is: the host
runs the same code far faster than the part. The useful column is the device
model, built from the operation counts the PoC records and from datasheet
DS5319 Table 23:

```
tprog   Word programming time    min 20 us   max 40 us
tERASE  Page (1kB) erase time    min 20 ms   max 40 ms
```

The **Typ column of that table is empty** — ST gives a min and a max and no
typical — so every F103 figure below is a range. The part programs 16 bits per
operation, so a 1 KB page is one erase plus 512 programming operations:
**30.2 ms best case, 60.5 ms worst**, and 1.57–3.14 s for a full 52 KB slot.

```
attempt                   host ms         F103 ms     of it flash   erases  prog bytes
1 run: session            101.079     417.4-568.7     151.3-302.7        6        3134
1 run: apply+verify         9.255     409.9-500.7      90.8-181.7        4        1084
1 run: trial 2              7.780      90.8-181.6      90.8-181.6        4        1080
1 run: revert              17.735     220.0-421.6     201.6-403.2        9        2160
4 runs: session           249.684     509.0-751.1     242.1-484.2        9        6212
12 runs: session          594.262    722.8-1176.7     453.9-907.9       16       13394
plain boot                  1.231       18.4-18.4         0.0-0.0        0           0
```

**Flash programming dominates, not cryptography.** A 12-sector update spends
454–908 of its 723–1177 ms in flash. On a 9600 baud link that is invisible next
to the transfer; on CAN FD or TCP it is the floor, and it scales with sectors
written.

**A trial boot costs 91–182 ms and does no useful work.** Every state change
rewrites a 4-page meta bank. With endurance at 1 kcycle minimum (DS5319 Rev 1;
10 kcycles later), that is an endurance cost as well as a time cost, which is
what justifies the attempt-token log in `DESIGN.md` section 2: clear one bit
per attempt instead of erasing a bank. The PoC does not implement that
optimisation, which is why the cost is visible here.

Timing also found a plain inefficiency: the apply path wrote the meta bank
twice, once to enter `TRIAL` and once to take the first attempt token. Merging
them saved a bank erase per update.

## Savings, both currencies

`scripts/overhead.sh` prints bytes and device time together. The full-image
baseline is computed from the frame and message sizes rather than measured, and
it lands on 60 724 B — exactly what a forced full transfer measures.

For a 5-sector change in steady state:

| | Delta | Full image | Saved |
|---|---|---|---|
| Wire | 7 943 B | 60 724 B | 52 781 B, 86.9 % |
| Sectors written | 5 | 52 | 47 |
| Flash time | 151–302 ms | 1 572–3 145 ms | 1.4–2.8 s |
| Total at 9600 baud | 8.4–8.6 s | 64.8–66.4 s | 56.2–57.8 s |
| Total at 115200 baud | 0.8–1.0 s | 6.8–8.4 s | 6.0–7.4 s |

**Copies save bytes but not flash time.** A `STEP_COPY` still erases and
programs the destination page, so it costs the device what a `STEP_WRITE`
costs. On a first update into an erased slot, rule 2 takes the wire from 87.1 %
to 15.8 % of a full image while flash stays at 1.36–2.72 s either way. On a
slow link that is close to free money; on a fast link it buys almost nothing,
because flash is the floor. Rule 1 — the sector that is already correct, so
neither sent nor written — is the only rule that saves both.

## What this PoC does not do

Deliberate gaps, so the parts that are modelled are modelled honestly:

* **No ECDSA and no TLS.** Both signatures — the device manifest signature and
  the release signature in the image descriptor — are HMAC-SHA256 with a key
  the server also holds. That exercises every verification path and every
  rejection path, but a shared secret is not the design: production uses ECDSA
  P-256 with RFC 6979 deterministic `k`, because the F103 has no hardware RNG.
  The transport is a plain socket, not TLS.
* **One build per release, not one per slot.** Real per-slot builds differ in
  their absolute addresses, so cross-slot copies would hit fewer sectors than
  they do here. `--no-copy` brackets the pessimistic end, and the steady-state
  rows show rule 2 contributing nothing anyway.
* **No transport adapters.** Only the byte-stream framing. Modbus and CAN
  (`DESIGN.md` section 16) are not implemented.
* **The device applies writes, the bootloader applies copies**, as designed, but
  the plan is persisted once at `PLAN_END` rather than incrementally. A crash
  mid-download therefore loses the plan, not the data — the device re-manifests
  and the server re-diffs, which is the simplification argued for in section
  15.4. `run_resume.sh` demonstrates it: after a power cut 20 sectors in, the
  second session skips 27 sectors and sends only the gap.
* **No real execution.** "Booting" prints the slot and version it would jump to.

## Things the PoC found in the design

Writing it turned up three mistakes in `DESIGN.md`, all now fixed there:

1. **The journal encoding could not work.** Section 2 specified
   `0xFFFF -> 0xAAAA -> 0x5555 -> 0x0000` while claiming bits only ever go from
   1 to 0. `0xAAAA -> 0x5555` sets every other bit, so it needs a page erase.
   Now `0xFFFF -> 0xFFF0 -> 0xFF00 -> 0x0000`, which really is monotonic. The
   emulated flash rejects any program that sets a bit, so this is now enforced
   rather than asserted.
2. **The manifest nonce broke manifest elision.** Section 4 put the server
   nonce inside the manifest body and hashed the body, so `manifest_sha256`
   changed every session and the cache in 17.1 could never hit. The body is now
   nonce-free and freshness lives only in `ManifestAuth`.
3. **Framing overhead was off by one.** The stated 11 B per frame is 10:
   2 magic + 2 length + 1 type + 1 sequence + 4 CRC. The section 17 table has
   been replaced with measured numbers, which also corrected the break-even
   claim: against a like-for-like baseline there is no crossover, and the worst
   case is a tie rather than a 3.7 % loss.
