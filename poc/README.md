# Host PoC

A working implementation of one update request from `DESIGN.md`, built with gcc
and run on the host. The device reports what is in its flash, the server sends
only the sectors that differ, and the bootloader applies them.

The device side owns a 128 KB file treated as STM32F103CB flash: page erase,
half-word programming, and the rule that a program may only clear bits. The
firmware under test is a real binary, `klipper.bin` (45 772 B), split into
sectors and wrapped in an image descriptor.

## Run it

```sh
make
scripts/run_demo.sh
```

Or drive it by hand. There is no setup step: the device is created on first use
with the reference binary in slot 0, so its first manifest describes real
firmware.

```sh
scripts/lab.sh server &        # or in its own terminal, without the &
scripts/lab.sh release 4       # publish a release with 4 patched runs
scripts/lab.sh update          # one request: manifest, delta, apply, confirm
scripts/lab.sh status
kill %1
```

The server stays up across requests and prints one stats row per request, so
`release` then `update` again adds another row. `scripts/lab.sh reset` forgets
the device so it ships fresh again.

## What a request looks like

```
server: HELLO active_slot=0 running_version=1 last_result=0
server: manifest received and verified, 1880 B of body
server: plan v2 into slot 1, 45 steps (7 skipped)
sectors: 45 of 52 differ; 5 sent, 40 copied on the device, 7 already right
  sent    0,8,19,41-42
  copied  1-7,9-18,20-40,43-44

case                       skip copy write     wire of full   saved B   saved      flash ms
-------------------------- ---- ---- ----- -------- ------- --------- ------- -------------
v2 into slot 1                7   40     5     9615   15.8%     51141   84.2%     1361-2722
v3 into slot 0               40    0    12    15836   26.1%     44920   73.9%       363-726
```

`wire` is every byte of the session, framing included. `of full` compares it
with the same protocol sending all 52 sectors, a baseline computed from the
frame and message sizes. `flash ms` is what the device spends erasing and
programming, from datasheet DS5319 Table 23: page erase 20–40 ms and half-word
program 20–40 µs, with an empty Typ column, so one 1 KB sector is 30.2–60.5 ms
and a full slot is 1.57–3.14 s.

Two things that table shows up:

**Copies save bytes, not flash time.** A copy still erases and programs the
destination page, so it costs the device what a sent sector costs. The first
row sends 5 sectors but writes 45, which is why its flash cost is near the full
image while its wire cost is 15.8 % of it. Rule 1 — the sector already correct,
so neither sent nor written — is the only rule that saves both.

**Sector-delta efficiency depends on code layout.** A change that shifts the
bytes after it invalidates every following sector. Reproducible builds and a
stable link order are what keep a one-line fix from costing a full transfer.

## The pieces

| Binary | Role |
|---|---|
| `cb-server --image FILE` | serves one release; re-reads the file each request |
| `cb-client --flash FILE --initial FILE` | the device: one request, start to finish |
| `cb-flash` | `manifest`, `map`, `mutate` (build a test release) |

| Source | Covers |
|---|---|
| `flash.c` | Page erase, half-word program with the bits-only-clear rule, meta bank ping-pong, journal |
| `manifest.c` | Canonical manifest body, digest, MAC, structural checks |
| `plan.c` | The three planner rules of `DESIGN.md` section 5 |
| `image.c` | Image descriptor at slot offset `0x200`, `image_sha256` with the descriptor hole, installing a slot |
| `frame.c` | `A5 5A \| len \| type \| seq \| payload \| crc32` framing and the byte counters |
| `client.c` | The request, then the bootloader: apply, verify, trial boot, confirm |

The flash map is `cb-flash map`, and it is the layout from `DESIGN.md`
section 2 with the identity page from 10.2:

```
bootloader          0-14     0x00000     15360
device identity    15        0x03c00      1024
meta bank 0        16-19     0x04000      4096
meta bank 1        20-23     0x05000      4096
slot 0             24-75     0x06000     53248
slot 1             76-127    0x13000     53248
```

## What this PoC does not do

It is one request on a healthy link. The design's recovery paths exist in the
code — the bootloader unrolls a plan whose image does not verify, and reverts a
trial boot that is never confirmed — but nothing here provokes them.

* **No ECDSA and no TLS.** Both signatures are HMAC-SHA256 with a key the
  server also holds. That exercises every verification and rejection path, but
  a shared secret is not the design: production uses ECDSA P-256 with RFC 6979
  deterministic `k`, because the F103 has no hardware RNG. The transport is a
  plain socket.
* **One build per release, not one per slot.** Real per-slot builds differ in
  their absolute addresses, so cross-slot copies would hit fewer sectors than
  they do here.
* **No transport adapters.** Only the byte-stream framing; Modbus and CAN
  (`DESIGN.md` section 16) are not implemented.
* **The plan is persisted once**, at the end of the download, rather than
  incrementally. A crash mid-download loses the plan but not the data: the
  device re-manifests and the server re-diffs, which is the simplification
  argued for in section 15.4.
* **No real execution.** "Booting" prints the slot and version it would jump to.

## Things the PoC found in the design

Writing it turned up three mistakes in `DESIGN.md`, all fixed there:

1. **The journal encoding could not work.** Section 2 specified
   `0xFFFF -> 0xAAAA -> 0x5555 -> 0x0000` while claiming bits only ever go from
   1 to 0. `0xAAAA -> 0x5555` sets every other bit, so it needs a page erase.
   Now `0xFFFF -> 0xFFF0 -> 0xFF00 -> 0x0000`, and the emulated flash rejects
   any program that sets a bit, so it is enforced rather than asserted.
2. **The manifest nonce broke manifest elision.** Section 4 put the server
   nonce inside the manifest body and hashed the body, so `manifest_sha256`
   changed every session and the cache in 17.1 could never hit. The body is now
   nonce-free and freshness lives only in `ManifestAuth`.
3. **Framing overhead was off by one.** The stated 11 B per frame is 10:
   2 magic + 2 length + 1 type + 1 sequence + 4 CRC.

It also replaced two invented flash timings with the datasheet's, which changed
the conclusion about what delta saves: see `DESIGN.md` section 17.
