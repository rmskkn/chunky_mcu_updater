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
with the reference binary in its one partition, so its first manifest
describes real
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

## Architecture

Two processes and one file. The server owns the releases; the device owns the
flash and is the only thing that writes it. Nothing else is shared.

```
  cb-server --image release.bin                     cb-client --flash flash.bin
  +-----------------------------------+             +-----------------------------------+
  | release.bin   raw firmware        |             | manifest.c  md5 of all 104        |
  |                                   |             |             sectors of the        |
  |                                   |             |             partition             |
  | image.c   wrap into an image:     |             | client.c    receive blobs, write  |
  |           descriptor at 0x200,    |             |             them straight into    |
  |           image_sha256, MAC       |             |             them in place         |
  |                                   |             |                                   |
  | plan.c    3 rules against the     |             | flash.c     erase page,           |
  |           device manifest:        |             |             program half-words,   |
  |           skip / copy / write     |             |             refuse to set a bit   |
  |                                   |             |                                   |
  | cache/    last manifest per       |             | client.c    bootloader: copies,   |
  |           device, for elision     |             |             verify, trial, confirm|
  +-----------------------------------+             +-----------------------------------+
                    |                                                 |
                    +------------ frame.c over TCP -------------------+
                                             |
                       | A5 5A | len | type | seq | payload | crc32 |
                         10 B of overhead, 256 B of sector per frame
```

The device never hands out its flash and the server never addresses it
directly: every decision the server makes comes from the manifest the device
chose to send.

```
                           flash.bin, 128 KB
  +--------------+------+---------+---------+---------------------------+
  | bootloader   | id   | meta 0  | meta 1  | application               |
  | 15 KB        | 1 KB | 4 KB    | 4 KB    | 104 KB, 104 sectors       |
  +--------------+------+---------+---------+---------------------------+
   pages 0-14     15     16-19     20-23     24-127
```

| Region | Who writes it | What it holds |
|---|---|---|
| bootloader | nobody, after provisioning | the update: receive, apply, verify |
| id | the factory | device key, root key; write-protected |
| meta 0 / meta 1 | the bootloader | plan state, the plan, the progress journal; two banks ping-pong by generation so a torn write loses nothing |
| application | the bootloader, in place | the one firmware image, read to build the manifest and rewritten sector by sector |

**One partition means there is nothing to fall back to.** An update rewrites
the running image, so a release that fails to verify leaves the device with no
bootable firmware; the bootloader says so and asks for another release. That
also settles who does the writing: the running image cannot overwrite itself,
so the update belongs to the bootloader, not the application. What survives a
power cut mid-update is resume, not rollback — the partition's own hashes are
the progress record, so the next request re-diffs and sends only the gap.

## One request, message by message

```
  device                                                      server
    |                                                             |
    |-- HELLO  id, geometry, version + image hash --------------->|
    |<-- CHALLENGE  16-byte nonce --------------------------------|
    |                                                             |
    |   md5 every sector of the partition (104 x 1 KB)            |
    |-- MANIFEST_DIGEST  sha256 of the body, MAC over the nonce ->|
    |<-- MANIFEST_NEEDED  only on a cache miss -------------------|
    |-- MANIFEST  1876 B of body, fragmented -------------------->|
    |-- MANIFEST_END  digest + MAC ------------------------------>|
    |                              verify MAC, nonce, structure   |
    |                      build the target, diff it per sector   |
    |<-- PLAN_BEGIN  version, steps, sha256 + MAC ----------------|
    |                                                             |
    |<-- STEP_WRITE  dst, offset, 256 B   (x4 per sector) --------|
    |<-- STEP_WRITE_END  dst, md5 --------------------------------|
    |   check md5, erase the page, program, re-read, check        |
    |-- STEP_ACK  dst ------------------------------------------->|
    |<-- STEP_COPY  dst, src index, md5 --------------------------|
    |   recorded only; the bootloader does the copying            |
    |-- STEP_ACK  dst ------------------------------------------->|
    |<-- PLAN_END ------------------------------------------------|
    |-- PLAN_READY  steps left for the bootloader --------------->|
    |<-- COMMIT --------------------------------------------------|
    |                                                             |
    |   arm the plan: clear one journal half-word, reboot         |
    |   bootloader: run the copies, re-check every md5,           |
    |               check image_sha256 and the signature          |
    |   verifies -> boot it; fails -> ask for another release     |
```

Sector 0 carries the descriptor and is always sent last, so an interrupted
request leaves the partition without a valid header: unmistakably not
bootable, rather than plausibly bootable and wrong.

## What a request looks like

```
server: HELLO running_version=1 last_result=0
server: manifest received and verified, 1876 B of body
server: plan v2, 5 steps (99 skipped)
sectors: 5 of 104 differ; 5 sent, 0 copied on the device, 99 already right
  sent    0,8,19,41-42
  copied  -

case                       skip copy write     wire of full   saved B   saved      flash ms
-------------------------- ---- ---- ----- -------- ------- --------- ------- -------------
v2                           99    0     5     7984    6.7%    111870   93.3%       151-302
v3                           89    0    15    19284   16.1%    100570   83.9%       454-907
```

`wire` is every byte of the session, framing included. `of full` compares it
with the same protocol sending all 104 sectors, a baseline computed from the
frame and message sizes. `flash ms` is what the device spends erasing and
programming, from datasheet DS5319 Table 23: page erase 20–40 ms and half-word
program 20–40 µs, with an empty Typ column, so one 1 KB sector is 30.2–60.5 ms
and the whole 104 KB partition is 3.15–6.29 s.

Two things that table shows up:

**Rule 2 barely fires with one partition.** A copy needs the bytes to already
exist somewhere in the flash the manifest covers, and with a single partition
that means elsewhere in the same image — which for real firmware is almost
never true. Both rows above copy nothing. The A/B design in `DESIGN.md` is
where rule 2 earns its keep, because the other slot holds a related release.

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
| `image.c` | Image descriptor at partition offset `0x200`, `image_sha256` with the descriptor hole, installing an image |
| `frame.c` | `A5 5A \| len \| type \| seq \| payload \| crc32` framing and the byte counters |
| `client.c` | The request, then the bootloader: apply, verify, trial boot, confirm |

The flash map is `cb-flash map`, and it is the layout from `DESIGN.md`
section 2 with the identity page from 10.2:

```
bootloader          0-14     0x00000     15360
device identity    15        0x03c00      1024
meta bank 0        16-19     0x04000      4096
meta bank 1        20-23     0x05000      4096
application        24-127    0x06000    106496
```

## What this PoC does not do

It is one request on a healthy link.

* **No ECDSA and no TLS.** Both signatures are HMAC-SHA256 with a key the
  server also holds. That exercises every verification and rejection path, but
  a shared secret is not the design: production uses ECDSA P-256 with RFC 6979
  deterministic `k`, because the F103 has no hardware RNG. The transport is a
  plain socket.
* **No rollback.** `DESIGN.md` sections 7 and 8 describe a trial boot, unroll
  and revert, all of which need a second partition. With one partition they do
  not exist: the device either verifies the new image or reports that it has
  none. Section 18 records the trade.
* **No transport adapters.** Only the byte-stream framing; Modbus and CAN
  (`DESIGN.md` section 16) are not implemented.
* **The plan is persisted once**, at the end of the download, rather than
  incrementally. A crash mid-download loses the plan but not the data: the
  device re-manifests and the server re-diffs, which is the simplification
  argued for in section 15.4.
* **No real execution.** "Booting" prints the version it would jump to.

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
