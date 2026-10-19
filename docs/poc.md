# Host PoC

A working implementation of one update request from `DESIGN.md`, built with gcc
and run on the host. The device reports what is in its flash, the server sends
only the sectors that differ, and the bootloader applies them.

The device side owns a 128 KB file treated as STM32F103CB flash: page erase,
half-word programming, and the rules that a program may only clear bits and
may not reprogram a half-word except with zero. The
firmware under test is a real binary, `klipper.bin` (45 772 B), split into
sectors and wrapped in an image descriptor.

## Run it

```sh
make                                  # needs liblz4
make ERASE_SIZE=2048 SECTOR_SIZE=4096 # other geometry, rebuilds everything
scripts/run_demo.sh
```

`cb-flash geometry` prints the build's geometry as a shell-sourceable line.
`tests/run_matrix.sh` runs the whole suite at four ERASE:SECTOR geometries;
`make test-compress` runs the compression API and LZ4 tests under ASan/UBSan.

Or drive it by hand. There is no setup step: the device is created on first use
with the reference binary in its one partition, so its first manifest
describes real
firmware.

```sh
scripts/lab.sh server &             # or in its own terminal, without the &
scripts/lab.sh release firmware.bin # publish your own binary as the release
scripts/lab.sh update               # ask for it: manifest, delta, apply, verify
scripts/lab.sh status
kill %1
```

Two halves to that, and they are independent.

**Specifying the release** is writing the file the server was started with. It
re-reads that file on every request, so publishing a new one needs no restart
and no signal: `release firmware.bin` copies your binary over it, and `release
12` synthesises one instead by patching 12 runs of the shipped firmware, which
is what a release touching a few functions looks like. Without the harness it is
just `cp firmware.bin run/release.bin`, or point the server somewhere else with
`cb-server --image /path/to/firmware.bin`.

**Triggering the update** is running the client. Nothing pushes: the device
asks, by hashing its partition and sending the manifest, and the server answers
with the sectors that differ. If the partition already holds that firmware the
answer is `no update offered` — the comparison is on content, so re-running
`update` is free and idempotent. `scripts/lab.sh reset` forgets the device so it
ships fresh again.

## Benchmarks

`tests/run_all.sh` output, one row per `tests/case_*.sh`, default geometry
(`DEVICE=stm32f103cb ERASE_SIZE=1024 SECTOR_SIZE=1024`, 8 B sector hashes, 52 224 B raw image =
51 sectors, one A/B slot).
`wire_stats` in `tests/common.sh` derives these numbers from the client's own
`device: wire tx=.. rx=.. bytes (framing overhead ..)` log line (`middleware/client_session.c`),
which counts real frame bytes, not an estimate. Reproduce with
`bash tests/run_all.sh`.

Bytes saved = the 52 224 B raw image minus rx, i.e. against a naive scheme
that reflashes the whole image every request. tx (manifest + auth, the device
telling the server what it already has) is not counted as savable — a naive
scheme would not pay it, so it is this approach's fixed overhead, not part of
what it saves.

| Case | What it exercises | tx | rx | rx framing overhead | rx vs raw image | bytes saved |
|---|---|--:|--:|--:|--:|--:|
| `case_copy_dedup` | `OP_COPY` plus one interleaved write | 712 B | 2345 B | 160 B (6.8%) | 4.5% | 49879 B (95.5%) |
| `case_copy_src_rewritten` | a copy whose source the plan rewrites: sent as a write instead | 712 B | 3435 B | 200 B (5.8%) | 6.6% | 48789 B (93.4%) |
| `case_lz4_compressible` | a repetitive (16-byte motif) sector | 698 B | 1281 B | 120 B (9.4%) | 2.5% | 50943 B (97.5%) |
| `case_lz4_dict` (step 2: insertion shifts 32 sectors) | LZ4 with the old sector as dictionary | 604 B | 2413 B | 680 B (28.2%) | 4.6% | 49811 B (95.4%) |
| `case_lz4_uniform_00` | a uniform `0x00` sector | 698 B | 1265 B | 120 B (9.5%) | 2.4% | 50959 B (97.6%) |
| `case_lz4_uniform_ff` | a uniform `0xFF` sector | 698 B | 1265 B | 120 B (9.5%) | 2.4% | 50959 B (97.6%) |
| `case_many_writes` | 17 ordinary `STEP_WRITE`s, no dedup/compression available | 908 B | 18643 B | 890 B (4.8%) | 35.7% | 33581 B (64.3%) |
| `case_noop_rerun` (2nd request) | device already matches the release: bare handshake, manifest elided | 144 B | 36 B | 20 B (55.6%) | 0.1% | 52188 B (99.9%) |

**Grand total, every `update()` request this run made (12, including
`case_lz4_dict`'s other three):** tx = 6938 B (1280 B framing, 18.4%),
rx = 35 930 B (2760 B framing, 7.7%) — **5.7%** of 626 688 B of
raw-image-equivalent traffic (12 requests × the 52 224 B raw image) —
**590 758 B saved, 94.3%**.

A request right after an update elides the manifest too: the server caches
the manifest the plan will produce, not only the one it received, so the
`noop_rerun` follow-up costs 180 B in both directions.

Framing overhead (the `| 0xA5 0x5A | len | type | seq | payload | crc32 |`
wrapper, `DESIGN.md` section 6) is a *larger* share of rx the smaller the
payload is — it's fixed per frame, so a near-empty response (`noop_rerun`,
55.6%) pays proportionally more than a 32-sector plan (`lz4_dict`, 28.2%).
That column is about where rx's own bytes go, not about savings.

The worst case here (`many_writes`, 64.3% saved) is the floor: 17 of 51
sectors changed with content that neither dedups nor compresses, and it
still cuts traffic by almost two thirds. The best realistic case (`copy_dedup`/
`lz4_*`, ~95-97%) matches `DESIGN.md`'s stated "typical release lands around
10–15% of a full image" (these synthetic cases do better because the patches
are small and either duplicate existing content or compress trivially).

Two processes and one file. The server owns the releases; the device owns the
flash and is the only thing that writes it. Nothing else is shared.

```
  cb-server --image release.bin                     cb-client --flash flash.bin
  +-----------------------------------+             +-----------------------------------+
  | release.bin   raw firmware        |             | manifest.c  sha256 of all 51      |
  |                                   |             |             sectors, 8 B each     |
  |                                   |             |             order implicit        |
  | image.c   wrap into an image:     |             | device.c    receive blobs, write  |
  |           descriptor at 0x200,    |             |             them straight into    |
  |           image_sha256, MAC       |             |             them in place         |
  |                                   |             |                                   |
  | plan.c    3 rules against the     |             | flash.c     erase page,           |
  |           device manifest:        |             |             program half-words,   |
  |           skip / copy / write     |             |             refuse to set a bit   |
  |                                   |             |                                   |
  | cache/    last manifest per       |             | device.c    bootloader: copies,   |
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
  +------------+------+--------+--------+--------------+--------------+
  | bootloader | id   | meta 0 | meta 1 | slot A       | slot B       |
  | 15 KB      | 1 KB | 5 KB   | 5 KB   | 51 KB, 51 s. | 51 KB, 51 s. |
  +------------+------+--------+--------+--------------+--------------+
   pages 0-14   15     16-20    21-25    26-76          77-127

  The layout comes from the device catalog, device/stm32f103cb/device.toml,
  which tools/gen_device.py turns into build/device/geometry.h and
  build/device/memory.ld (make DEVICE=...). Each meta bank is 4 KB of meta_t plus the journal's
  own page.
```

| Region | Who writes it | What it holds |
|---|---|---|
| bootloader | nobody, after provisioning | the update: receive, apply, verify |
| id | the factory | device key, root key; write-protected |
| meta 0 / meta 1 | the bootloader | plan state, the plan, the progress journal; two banks ping-pong by generation so a torn write loses nothing |
| slot A | the bootloader, in place | the firmware image, read to build the manifest and rewritten sector by sector |
| slot B | nobody yet | reserved for the A/B flow (`DESIGN.md` sections 2-8); the PoC still updates slot A in place |

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
    |   sha256 every sector of the partition (51 x 1 KB)          |
    |-- MANIFEST_DIGEST  sha256 of the body, MAC over the nonce ->|
    |<-- MANIFEST_NEEDED  only on a cache miss -------------------|
    |-- MANIFEST  body, fragmented (size: see DESIGN.md sec. 18) >|
    |-- MANIFEST_END  digest + MAC ------------------------------>|
    |                              verify MAC, nonce, structure   |
    |                      build the target, diff it per sector   |
    |<-- PLAN_BEGIN  version, steps, sha256 + MAC ----------------|
    |                                                             |
    |   every step streamed with no per-step wait:                |
    |<-- STEP_WRITE  dst, offset, <=256 B of the encoded sector --|
    |<-- STEP_WRITE_END  dst, compress id, enc_len, hash ---------|
    |<-- STEP_COPY  dst, src index, hash -------------------------|
    |   ... (one of the two, once per step, until the plan ends)  |
    |<-- PLAN_END ------------------------------------------------|
    |                                                             |
    |   per STEP_WRITE_END: decompress, check the hash, then      |
    |   erase and program the sector before taking the next       |
    |   message - the link's own buffering absorbs the stall;     |
    |   per STEP_COPY: recorded only, the bootloader copies later |
    |   each step's ACK/NAK follows once its work is done, in the |
    |   order the steps were sent:                                |
    |-- STEP_ACK/NAK  dst  (one per step, same order as above) -->|
    |                                                             |
    |-- PLAN_READY  steps left for the bootloader --------------->|
    |<-- COMMIT --------------------------------------------------|
    |                                                             |
    |   arm the plan: clear one journal half-word, reboot         |
    |   bootloader: run the copies, re-check every hash,          |
    |               check image_sha256 and the signature          |
    |   verifies -> boot it; fails -> ask for another release     |
```

Sector 0 carries the descriptor and is always sent last, so an interrupted
request leaves the partition without a valid header: unmistakably not
bootable, rather than plausibly bootable and wrong.

## What a request looks like

```
server: HELLO running_version=1 last_result=0
server: manifest received and verified, 836 B of body
server: plan v2, 6 steps (98 skipped)
sectors: 6 of 104 differ; 6 sent, 0 copied on the device, 98 already right
  sent    0,3,5,7,16,34
  copied  -

case                       skip copy write     wire of full   saved B   saved      flash ms
-------------------------- ---- ---- ----- -------- ------- --------- ------- -------------
v2                           98    0     6     7992    6.8%    109956   93.2%       181-363
```

`wire` is every byte of the session, framing included. `of full` compares it
with the same protocol sending all 104 sectors, a baseline computed from the
frame and message sizes. `flash ms` is what the device spends erasing and
programming, from datasheet DS5319 Table 23: page erase 20–40 ms and half-word
program 20–40 µs, with an empty Typ column, so one 1 KB sector is 30.2–60.5 ms
and the whole 104 KB partition is 3.15–6.29 s.

Two things that table shows up:

**The manifest is hashes and nothing else.** One 8-byte hash per sector, order
implicit, so 104 sectors cost 836 B. Compressing it is not worth it: measured
here, `zstd -19` halves the older 18-byte-per-entry form, but only because 59
of 104 sectors are an erased page and hash alike; on a full partition the same
test *grows* the file, since hash output has no structure to exploit. Encoding
beat compression, and the digest in `DESIGN.md` 17.1 beats both when the
server's cache is warm.

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
| `cb-flash` | `manifest`, `find` (which sector holds a hash), `map`, `geometry`, `mutate` (build a test release) |

| Source | Covers |
|---|---|
| `flash.c` | Page erase, half-word program with the bits-only-clear and one-program-per-erase rules, meta bank ping-pong, journal |
| `manifest.c` | Canonical manifest body, digest, MAC, structural checks |
| `plan.c` | The three planner rules of `DESIGN.md` section 5 |
| `image.c` | Image descriptor at partition offset `0x200`, `image_sha256` with the descriptor hole, building an image |
| `flash_view.c` | Reading the image and the manifest out of the device's flash, apart from the RAM-only code so a server links no flash port |
| `compress.c` | Compressor registry; nothing built in. Sectors go out raw, LZ4, or LZ4 with the old sector as dictionary, whichever is smallest |
| `protocol.h` | The wire protocol on its own: frame layout, message types and records, reason and result codes. No flash model or geometry |
| `device.c` | The device core, fed one message at a time with no threads, transport or clock: the request, then the bootloader: apply, verify, trial boot, confirm |
| `middleware/frame.c` | `A5 5A \| len \| type \| seq \| payload \| crc32` framing and the byte counters |
| `middleware/client_session.c`, `middleware/server_session.c` | Each side's session carried over a framed link |
| `middleware/flash_file.c` | The host's flash port: the part as an mmap'd file. Create, provision, install the factory image, open by path. `lib/` only sees an opaque port |
| `middleware/lz4.c` | The liblz4 binding, registered by each CLI with `lz4_register()` |
| `middleware/host.c` | Files, randomness, clock, environment: what `lib/` never does |

The flash map is `cb-flash map`. It needs no flash file: it walks
`CB_REGION_LIST` from the generated `build/device/geometry.h`, which any
firmware or tool can include on its own (it depends on nothing but
`<stddef.h>`). This is the layout from `DESIGN.md` section 2 with the
identity unit from 10.2, at the default 1 KB / 1 KB geometry; slot A is
updated in place, slot B is reserved:

```
STM32F103CB, erase unit 1024 B, sector 1024 B, sector hash 8 B
region       units      offset    address       size  protect
bootloader     0-14     0x00000   0x08000000   15360  yes
identity      15-15     0x03c00   0x08003c00    1024  yes
meta0         16-20     0x04000   0x08004000    5120  no
meta1         21-25     0x05400   0x08005400    5120  no
slot_a        26-76     0x06800   0x08006800   52224  no
slot_b        77-127    0x13400   0x08013400   52224  no
```

The server keeps a sector store next to its manifest cache,
`<cache>/sectors/<hash>`, one file per sector it has served. A stored sector
is the dictionary for the same sector's next version, which is why a code
shift costs about 1 % of the raw bytes. Entries are re-hashed on read, so a
damaged file is just a miss.

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
   Bits-only-clear turned out not to be the binding rule either: an F103 also
   refuses to reprogram a half-word that is not `0xFFFF`, except with `0x0000`,
   so the replacement `0xFFFF -> 0xFFF0 -> 0xFF00 -> 0x0000` would have raised
   `PGERR` on its middle step. The encoding is now the three states a half-word
   really has, `0xFFFF -> 0xFF00 -> 0x0000`, and the emulated flash enforces
   both rules rather than asserting them.
2. **The manifest nonce broke manifest elision.** Section 4 put the server
   nonce inside the manifest body and hashed the body, so `manifest_digest`
   changed every session and the cache in 17.1 could never hit. The body is now
   nonce-free and freshness lives only in `ManifestAuth`.
3. **Framing overhead was off by one.** The stated 11 B per frame is 10:
   2 magic + 2 length + 1 type + 1 sequence + 4 CRC.

It also replaced two invented flash timings with the datasheet's, which changed
the conclusion about what delta saves: see `DESIGN.md` section 17.

## Looking a sector up

The two directions are not symmetric, and that is the point of the encoding.

**Sector to hash is a subscript.** Order is implicit, so sector `i` is at
`h[i]`: `manifest_hash_at(m, i)`, no search.

**Hash to sector is a scan, and may match several sectors.** An erased page
hashes the same wherever it sits, so on this device 59 of 104 sectors share one
hash. `manifest_find(m, hash, &index)` returns the first match, which is
correct for its only caller: the planner asking "is there a sector that already
holds these bytes" for a copy source, where any match is as good as another.
That is what makes rule 2 a content lookup rather than a positional one.

To see it from the outside:

```sh
$ cb-flash find run/flash.bin 271744fddbf0966b
sector   0  271744fddbf0966b  at 0x06000
1 of 104 sectors match 271744fddbf0966b

$ cb-flash find run/flash.bin 9a8918b11878da50 | tail -2
sector 103  9a8918b11878da50  at 0x1fc00
59 of 104 sectors match 9a8918b11878da50      # the erased-page hash

$ cb-flash find run/flash.bin 3715
sector  41  371549cd779cf34a  at 0x10400
1 of 104 sectors match 3715 (prefix)
```

It takes a prefix, prints the flash address of every match, and exits non-zero
when nothing matches. `cb-flash manifest` prints the whole table if you would
rather grep it.

Note what the old encoding did *not* buy here: when an entry carried its own
`index`, hash-to-sector was still a scan of every entry — the index field only
told you where the matched entry claimed to live. Making the position the index
removed the field and the possibility of it disagreeing with reality.
