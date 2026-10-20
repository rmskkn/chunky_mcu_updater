# chunky protocol, format 2

Status: **draft** (PROTO-1), for review. Not yet agreed upstream.

This is the wire protocol of chunky in octets: what a device reports (the manifest), what a server sends (the
plan) and what an image carries (the descriptor), and what a receiver checks, in which order. It is written so
that a second implementation (a C bootloader, a server in another language) can be built from it alone and
checked against the vectors in [`rust/chunky_format/vectors`](rust/chunky_format/vectors).

The words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119. Where this text and the code differ, it is a
bug in one of them; the reference implementation is `chunky_format` (the types), `chunky` (the device) and
`chunky_server` (the planner); `chunky_format` is in [`rust/`](rust), the other two in [romixlab/chunky_mcu_updater](https://github.com/romixlab/chunky_mcu_updater).
[SECURITY.md](https://github.com/romixlab/chunky_mcu_updater/blob/main/SECURITY.md) says why the rules are what they are.

## 1. Layers

| Layer | What | Where |
|---|---|---|
| 0 | Octets: descriptor, plan header, steps, manifest; the rules a receiver applies to them | this document, §3 to §7 |
| 1 | Delivery: which node receives a plan, applies or stores it, and how it resumes | §8, §9 |
| 2 | Carrier: messages, sessions, flow control, segmentation | the transport; for WireWeaver `ww_dfu` (WireWeaver STD-12), §10 |

Layer 0 is the same on every device and every transport. It is generic over the flash: the erase unit, the
write unit, the chunk size and the partition sizes are parameters of a **geometry** (§2), never constants of the
format.

## 2. Terms

- **Chunk**: the unit of the delta and of the hash, `chunk` bytes. A write unit divides it; it divides an erase
  unit or equals one.
- **Erase block**: `erase` bytes, the unit of an erase.
- **Write unit**: what the flash programs at once; programmed once after an erase.
- **Slot** (ACTIVE, DFU): a span of `slot_len` bytes holding one image, a multiple of `erase`. ACTIVE is what
  runs; DFU is where an update is written (A/B). In place (§8) there is only one slot, written as it runs.
- `chunks = slot_len / chunk`, `blocks = slot_len / erase`, `chunks_per_block = erase / chunk`.
- **Image**: the whole slot: payload, `0xFF` padding, and the 128-byte descriptor in its last 128 bytes.
  `image_len = slot_len - 128` is what the image hash covers.
- **Geometry id**: a `u32` that names a geometry (sizes and partition table). Device and server MUST agree on it;
  a mismatch is refused, never adapted to. chunky derives it as FNV-1a 32 over the little-endian bytes of 13
  `u32` words: family, backend, chunk, erase, write, then offset and length of STATE, FLOOR, ACTIVE and DFU
  (`FlashGeometry::id`). Other implementations MAY derive it differently: only equality is tested.
- **Chunk hash**: the first 8 bytes of SHA-256 over the chunk's `chunk` bytes as they lie in flash. The length is
  fixed by the format number, not a parameter: 8 bytes in format 2; another length is another format.
- **Floor**: the device's anti-rollback number; an image whose security counter is below it is refused.

## 3. Encoding

- Every multi-byte integer is **little-endian**, unsigned: `u16`, `u32`.
- Byte arrays (`[N]`) are copied as they are.
- No padding, no alignment, no length prefixes except the one in a WRITE. Every field sits at a fixed offset
  given below.
- **One encoding per value.** There are no optional fields; reserved bytes MUST be zero and are checked. An
  unknown magic, format, op, scheme or entry tag is refused. A reader never skips what it does not know.
- Magic numbers are ASCII: `CKY2` descriptor, `CKP2` plan, `CKM2` manifest. The format byte is `2` in all three.
  A new format changes both the magic and the format byte; a receiver supports the formats it was built for and
  refuses the rest.

The types are defined once, in `chunky_format` (shrink_wrap `fixed_layout` and `head_tail` types). The C reader
`rust/chunky_format/c/chunky_format.h` is generated from them and has every offset and length below as a `#define`
(`CHUNKY_DESCRIPTOR_BODY_PRODUCT_ID_OFFSET`, ...).

## 4. Image descriptor (128 bytes)

The last 128 bytes of every image, and bytes 17..145 of every plan header: a 64-byte **body** and a 64-byte
**seal**.

### 4.1 Body

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 4 | magic | `CKY2` |
| 4 | 1 | format | `2` |
| 5 | 1 | scheme | `1` Ed25519, `2` unsigned |
| 6 | 2 | reserved | zero |
| 8 | 4 | product id | `u32`, equal to the device's |
| 12 | 4 | version | `u32`, informational (the server compares it, the device reports it) |
| 16 | 4 | security counter | `u32`, at or above the device's floor |
| 20 | 4 | image length | `u32`, equal to `image_len` of the device's geometry |
| 24 | 4 | geometry id | `u32`, equal to the device's |
| 28 | 32 | image SHA-256 | SHA-256 of the image bytes `[0, image_len)` |
| 60 | 4 | reserved | zero |

The image hash covers the whole slot before the descriptor, `0xFF` padding included: nothing in a slot is
unsealed.

### 4.2 Seal

The seal is over the **domain** followed by **exactly the 64 bytes of the body**:

```
message = "chunky image descriptor v2" || body[0..64]
```

The domain is the 26 ASCII bytes shown, without a terminator or a length. It makes a signature over a
descriptor useless for anything else, and a new descriptor format gets a new domain.

| Scheme | Seal (64 bytes) | Check |
|---|---|---|
| 1 Ed25519 | the Ed25519 signature (RFC 8032, 64 bytes) of `message` by the release key | strict verification (`verify_strict`: canonical `S`, no small-order `R` or key) with the public key the device holds |
| 2 unsigned | `SHA-256(message)`, then 32 zero bytes | equality of all 64 bytes |

A device is built for exactly one scheme (a signed build, or the explicit unsigned opt-out for small flash) and
checks the seal its own way. A body that names the other scheme is refused before its seal is checked
(`WrongScheme`, §4.3); one that names the device's scheme but carries the other kind of seal fails the seal (on a
signed device as a bad signature, on an unsigned device as a bad hash). Either way a signed device cannot be talked
down to unsigned.

### 4.3 Refuse cheaply, then verify, then decode

A receiver checks a descriptor in three stages. The first needs no key and can only refuse; the seal comes
before any field is decoded and used; the fields that decide whether the image is acceptable are read only from
the sealed body.

**Stage 1, refusals on the raw body bytes**, read at their offsets (§4.1) from the 64 bytes as received:

1. Magic `CKY2`. Failure: `BadMagic`.
2. Format `2`. Failure: `BadFormat`.
3. Scheme: the device's scheme passes; the other known scheme is `WrongScheme`; any other value `BadFormat`.
4. Both reserved fields zero. Failure: `Reserved`.
5. Image length equals `image_len` of the device's geometry. Failure: `BadLength`.

These checks MUST only refuse: passing them is never a reason to accept a descriptor or to skip the seal. Their
purpose is that junk (erased flash, zeros, a stray header) never costs a signature verification. Product id,
geometry id, version, security counter and image hash MUST NOT be read before stage 2.

**Stage 2, the seal** (§4.2) over the 64 body bytes **as received** (from the plan header or from flash).
Failure: `BadSignature`.

**Stage 3, decode and decide.** The fields are decoded from those same bytes, not from a copy made before the
seal was checked; there is no other way to get the fields of a descriptor on a device. Framing, scheme and length
are checked again from the decoded body (they passed stage 1, so this never fails on its own; it keeps the
decision on sealed values only). Then:

1. Product id. Failure: `WrongProduct`.
2. Geometry id. Failure: `GeometryMismatch`.
3. Security counter ≥ floor. Failure: `RolledBack`.

Checking an image in a slot is this, then the SHA-256 over `[0, image_len)` against the body's image hash
(`HashMismatch`). A read that faults on ECC flash makes the image unreadable, never a fault.

A descriptor that passes stage 1 and fails the seal costs one signature verification; anyone who can reach the
device can make it spend that, and could waste its time anyway.

## 5. Plan

A plan is a 145-byte **header** followed by a **body** of `body length` bytes: `step count` steps back to back,
each WRITE followed directly by its data. The stream is exactly `145 + body length` bytes.

### 5.1 Header (145 bytes)

| Offset | Size | Field | Rule |
|---|---|---|---|
| 0 | 4 | magic | `CKP2` |
| 4 | 1 | format | `2` |
| 5 | 4 | geometry id | `u32`, equal to the device's |
| 9 | 4 | step count | `u32`, `≤ blocks + chunks` |
| 13 | 4 | body length | `u32`, bytes after the header, `≤ blocks × 3 + chunks × (13 + chunk)` |
| 17 | 128 | descriptor | the target image's descriptor (§4), byte for byte the one that ends the image |

The header's own fields are **outside the seal**. They can only make a plan refused: the geometry id must match,
the two counts are bounded by the geometry up front and must be met exactly at the end.

### 5.2 Steps

A step is a one-byte **op**, then the fixed **head** of that op; a WRITE's data follows its head.

| Op | Name | Bytes | Layout after the op byte |
|---|---|---|---|
| `1` | ERASE | 3 | block `u16` |
| `2` | COPY | 13 | dst `u16`, src `u16`, hash `[8]` |
| `3` | WRITE | 13 + count | dst `u16`, count `u16`, hash `[8]`, then `count` data bytes |

- **ERASE** erases erase block `block` of the slot being written.
- **COPY** programs chunk `dst` of the slot being written with the bytes of chunk `src` of ACTIVE, which MUST
  have chunk hash `hash`. Zero bytes on the wire for a chunk the device already holds anywhere.
- **WRITE** programs chunk `dst` with the `count` bytes that follow, which MUST have chunk hash `hash`.
  `count` MUST equal `chunk`: a WRITE carries exactly one chunk.

Offsets within a step, op byte at 0: ERASE block at 1; COPY dst 1, src 3, hash 5..13; WRITE dst 1, count 3,
hash 5..13, data 13..13+count. The longest head is 13 bytes.

### 5.3 Framing a stream

A receiver that takes the body as a byte stream frames each step from its first bytes, before the data of a
WRITE needs a buffer:

1. Read the op byte. An op other than 1, 2, 3 is refused.
2. Read the rest of the head: 2 bytes (ERASE) or 12 (COPY, WRITE).
3. For a WRITE, read `count` at step offset 3 and refuse it **unless it equals `chunk`**, before reading any
   data. A receiver therefore never needs more than `13 + chunk` bytes of step buffer, whatever the stream says.
4. The step is `3`, `13` or `13 + chunk` bytes. It MUST NOT run past the body length the header announced.

Bytes after `145 + body length` are not part of the plan; a receiver refuses them or ignores the carrier's
remainder, but never reads them as steps. In chunky: `chunky_format::step_len_from_head` and
`chunky::Applier::step_len`; a receiver that gets one step per message hands the engine exactly that step's
bytes, and a trailing byte or two steps in one message are refused.

### 5.4 Order

Each step has a **position**: an ERASE of block `b` is at `2 × b × chunks_per_block`; a COPY or WRITE to chunk
`c` is at `2 × c + 1`. Every step's position MUST be strictly greater than the position of the step before it.
So:

- steps go up through the slot, an erase before the programs of its block;
- each block is erased at most once and each chunk programmed at most once per plan;
- the step count is bounded by `blocks + chunks`.

A block need not be erased if every chunk the plan programs in it already reads blank (§6.3).

### 5.5 What applying a plan does

The receiver writes only the slot being written (DFU in A/B), and only its `slot_len` bytes. COPY sources are
read from ACTIVE, which no step writes (A/B; for in place see §8.3).

## 6. Receiver checks, in order

A receiver that applies (or stores, §8.2) a plan MUST make these checks, and MUST NOT touch flash before
everything in §6.1 has passed. A failed check stops the plan with a named error; what is already written stays,
and the next manifest tells the server where things stand.

### 6.1 Begin (the header)

First every refusal that needs no key, then the seal, then the decisions on the sealed body. A junk header never
costs a signature verification.

1. Refusals on the header, which is outside the seal (refuse-only):
   1. Magic `CKP2` and format `2`.
   2. Geometry id equals the device's (`GeometryMismatch`, on both sides: the server refuses to plan for an
      unknown or mismatched geometry).
   3. Step count `≤ blocks + chunks` (`TooManySteps`).
   4. Body length `≤ blocks × 3 + chunks × (13 + chunk)` (`BodyLength`).
2. Refusals on the raw descriptor body at offset 17 (§4.3 stage 1, refuse-only): magic, format, scheme, reserved
   bytes, image length.
3. The seal over the exact 64 body bytes (§4.3 stage 2, `BadSignature`).
4. Decode the body from those bytes and decide (§4.3 stage 3): framing, scheme and length again, then product,
   geometry, floor.
5. The device may take an update now: in A/B it runs a confirmed image (or one a failed update reverted to), not
   a trial, and nothing is staged (`NotConfirmed`).

### 6.2 Each step

1. A step is still owed: fewer than `step count` steps done (`TooManySteps`).
2. Op known, head complete, the bytes exactly one step (§5.3; malformed, `BadData`).
3. The step fits in what is left of the body length (`BodyLength`).
4. A WRITE's count equals `chunk` (`BadData`).
5. Indices in range: `block < blocks`, `dst < chunks`, `src < chunks` (`OutOfRange`).
6. Position strictly after the previous step's (§5.4, `OutOfOrder`).
7. Then by op:
   - ERASE: erase the block.
   - COPY: the destination chunk reads blank (`NotBlank`; a chunk that faults on ECC is not blank); the source
     reads without an ECC fault (`SourceUnreadable`) and has the step's hash (`SourceMismatch`); program it; read
     it back and check the hash (`VerifyFailed`).
   - WRITE: the data has the step's hash (`DataMismatch`); the destination reads blank (`NotBlank`); program it;
     read back (`VerifyFailed`).

A chunk is only ever programmed when it reads blank, so no write unit is programmed twice, whatever the plan says.

### 6.3 Finish

1. Exactly `step count` steps were applied (`Incomplete`) and exactly `body length` bytes consumed
   (`BodyLength`).
2. The image now in the slot verifies (§4.3 and the image hash).
3. Its descriptor body is byte for byte the body the header announced (`WrongImage`).
4. Only then is the image marked staged (A/B) or bootable (in place).

The per-chunk hashes decide what is transferred and nothing else. Whether the slot holds the release is decided
by the full SHA-256 under the seal, here, before a swap and on every boot. With 8-byte chunk hashes a collision
(about 2^32 work for someone who controls both images) can make the planner skip or COPY a wrong chunk; that
fails step 2 every time (liveness, not integrity), and a planner SHOULD then fall back to WRITE for every chunk
(SRV-5).

## 7. Manifest

What a device reports so the server can plan: a 13-byte header, then one entry per chunk, `chunks` entries for
ACTIVE and then `chunks` entries for the update slot (DFU), each in chunk order. The order is implicit.

### 7.1 Header (13 bytes)

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | magic `CKM2` |
| 4 | 1 | format `2` |
| 5 | 1 | backend: `1` swap (bootloader copies), `2` bank (hardware remap), `3` RP2350 boot ROM |
| 6 | 1 | scheme the device accepts (§4.2) |
| 7 | 4 | geometry id `u32` |
| 11 | 2 | chunks `u16` |

### 7.2 Entries

| Tag | Bytes | Meaning |
|---|---|---|
| `0` | 9 | readable data; tag then the 8-byte chunk hash |
| `1` | 1 | blank: every byte reads `0xFF` |
| `2` | 1 | unreadable: faults on ECC (a write or erase cut by power loss); needs an erase |

A blank or unreadable chunk has no hash bytes, so an entry cannot say both. A parser takes exactly `2 × chunks`
entries and nothing after them.

The manifest is **unsigned**: layer 0 gives it no seal and no MAC. It is **untrusted** on the server: a hint for
the planner. A wrong one only wastes a transfer, because
every COPY and WRITE is checked on the device. It is computed fresh from flash for every session and is the
device's only progress record in A/B (§9).

### 7.3 The planner's rules (informative for A/B)

Per erase block of the slot being written: every chunk already right, no step; every wrong chunk blank (and blank
trusted), program just those; otherwise erase the block and program every chunk the target does not have blank.
A chunk is programmed by COPY when ACTIVE holds the same bytes anywhere, else by WRITE. Trusting blank chunks
saves the re-send of a block after a cut; chunky trusts them where a chunk is smaller than an erase unit and
re-erases where a chunk is one (an erase cut by power loss can read blank and still be marginal).

## 8. Delivery modes

Layer 0 is the same in all three; what differs is who receives the plan, where it is applied and what survives a
cut. `ww_dfu` names them by scenario code (`2C`, `2R`, `1S`, `1L`).

### 8.1 A/B, applied live (`2C`, `2R`)

The running application receives the plan and applies it into DFU as it arrives (§6). Nothing it runs from is
touched. At finish the image is staged; at the next reset the bootloader checks it again (§4.3, the bootloader
does not believe the application), switches to it for one trial boot, and reverts if it does not confirm. COPY
sources are ACTIVE, the running image. Implemented in chunky (`chunky_swap`, `chunky_bank`).

### 8.2 In place, from a stored plan (`1S`)

One slot. The running application receives the whole plan, WRITE data included, into a **plan area**, and keeps
running. **The stored form is the wire form**: the plan area holds the header and body bytes exactly as received,
so the bootloader reads them with the same parser.

- On receipt the application makes the checks of §6.1 and, per step, those of §6.2 that need no flash write
  (framing, counts, ranges, order, WRITE data hash), and checks that the plan fits the plan area.
- The bootloader trusts nothing the application wrote: before it erases anything it makes §6.1 again over the
  stored header and checks every stored step's framing, ranges, order and WRITE hash. Then it applies in place.
- No revert: once applying starts, the old image is gone. A plan that fails while applying leaves no bootable
  image; the device then needs another update path (recovery).

Not implemented in chunky (BOOT-6).

### 8.3 In place, applied live (`1L`)

One slot. The application reboots into an **updater** (the bootloader with a transport, or a small
write-protected recovery app at its own address) which receives the plan and applies it in place as it arrives.
From the first erase until finish the slot is not bootable; the bootloader sees that and starts the updater
again after any reset. Not implemented in chunky (BOOT-6).

### 8.4 The in-place planner rules

In both in-place modes the slot written is the slot COPY reads from. A planner for an in-place device MUST:

1. **Not COPY from a chunk the plan has already changed.** The simplest conforming planner emits no COPY in place
   (on real firmware it almost never fires there). A planner that does MUST take a source only from an erase
   block strictly after the destination's block, so neither an earlier step nor the destination block's own
   erase has touched it, and so it is still intact when the step is redone after a cut (§9.3).
2. **Erase every block it programs** (no trusted blank chunks): a stored plan must be able to redo a block from its
   erase after a cut, without a new plan.
3. Program the chunk holding the descriptor last; this falls out of ascending order, so the slot does not verify
   until the update is whole.

## 9. Resume

### 9.1 The flash is the progress record

There is no journal in A/B. After a lost link, a reset or a power cut the device computes a fresh manifest
(§7), the server plans again, and chunks already right are not sent again. A chunk cut while it was being
programmed reads as wrong data or unreadable, and its block is erased by the new plan. This always works and
needs no state on either side.

### 9.2 Session resume

A carrier MAY keep a session across a lost link: the session is identified by its 145 header bytes, byte for
byte. A receiver that still holds the state of an unfinished session (steps done, body bytes consumed, the
descriptor it opened) and gets the same header again continues from there and says from which step (per-step
delivery) or which body byte (bulk) to go on. A different header starts a new session.

- A step the receiver has already applied, sent again because its reply was lost, is acknowledged and not
  applied a second time. This is the carrier's rule; the layer 0 order (§5.4) refuses it. chunky's engine does
  not do this yet (APPLY-2).
- The step in flight at a break is redone from its first byte.
- In A/B the session lives in RAM: after a reset it is gone, and §9.1 applies.
- Resuming a session in chunky (an entry point from a verified prefix of the plan) is APPLY-2.

### 9.3 Resume after a power cut, in place

An in-place receiver MUST keep, in flash, what it needs to finish (the session record: header and the next step,
or for a stored plan an apply journal of one write-once flag per step). After a cut it restarts at the **ERASE
of the block that was in flight**, not at the step in flight: a cut program leaves a chunk that is neither blank
nor right, and only an erase makes it programmable again (§8.4 rules 1 and 2 make that restart safe). For a
stored plan the data is still in the plan area; a live updater asks the host for the steps again from there.

## 10. Carriers and segmentation

Layer 0 defines the bytes of a header, of each step and of the body stream. How they cross a link is the
carrier's:

- **Per-step delivery**: one message per step, answered with success or a named error. The receiver sees exactly
  one step at a time (§5.3). Fast links (USB, Ethernet, RTT).
- **Bulk delivery**: the header once, then the body as a byte stream, one acknowledgement at the end. Slow or
  metered links (LoRa, LTE-M, satellite), and the stored mode (§8.2), where the stream is written to the plan area
  unchanged.
- **Segmentation is the carrier's.** A carrier splits the header, a step or the stream into its frame size and
  delivers the bytes in order: classic CAN 8 bytes per frame, Modbus at most 253 bytes per PDU, `ww_dfu` pieces of
  `max_piece` with an offset. A WRITE of a 1 KB chunk does not fit one frame of either; the carrier reassembles
  it, or hands it on in pieces to a receiver that hashes as they come. Layer 0 neither aggregates several steps
  into one frame nor requires frames to align with steps.
- A receiver that programs a WRITE piece by piece, before the whole chunk has arrived and its hash checked, MUST
  check the hash on read-back and treat a mismatch like a cut program (§9.3).
- Confidentiality and authentication of the link (TLS, a secure channel, or none) are the carrier's and not part
  of this specification. Layer 0 is safe over a link that has neither: the image is trusted by its seal alone.
- Integrity of a link (CRC) is the carrier's and is not a trust boundary: every step is checked by its hash and
  the image by its seal.

The WireWeaver carrier is `ww_dfu` (WireWeaver STD-12): `Stage::begin` / `StageBulk::begin_bulk` take the 145
header bytes (`MAX_HEADER_LEN` 256), `Stage::erase` / `copy` / `write` carry the fields of one step with its index,
`StageBulk` carries the body as `PlanSegment { offset, data }` on a credit sink, and `Inventory` returns the
manifest.

## 11. Vectors

[`rust/chunky_format/vectors`](rust/chunky_format/vectors) holds a fixed firmware pair and the update between
them, made by one generator (`chunky_server::vectors` in [romixlab/chunky_mcu_updater](https://github.com/romixlab/chunky_mcu_updater)). `chunky_server/tests/vectors.rs` fails when the files
differ from what it makes; `chunky_sim/tests/vectors.rs` applies the plans to a simulated device and boots the
result; `chunky_format/tests/golden.rs` reads them with both Rust readers and `c/test.c` with the C reader
(`make vectors`, `rust/chunky_format/c/golden.h`).

Geometry: STM32G0 rules, erase block 2048, write unit 8, chunk 256, a 4096-byte slot (2 blocks, 16 chunks), swap
backend; geometry id `0x68b7e201`. Product `0x434b5931`. The device runs `old`; its DFU holds leftovers in block 0
and is blank in block 1. The set is made twice:

- `signed/`: sealed with Ed25519 by the throwaway key with seed `[7; 32]` (never a real key).
- `unsigned/`: the unsigned scheme (§4.2).

| File | Bytes | What |
|---|---|---|
| `old.bin` | 4096 | the running image (ACTIVE) |
| `new.bin` | 4096 | the target image |
| `dfu_before.bin` | 4096 | DFU before the update |
| `manifest.bin` | 165 | the device's manifest (§7): 13-byte header, 32 entries (15 hashes of 9 bytes, 17 blank of 1) |
| `plan.bin` | 1020 | the plan from `old` to `new` (§5) |
| `descriptor.bin` | 128 | the descriptor of `new` (its last 128 bytes, and plan bytes 17..145) |

`signed/plan.bin`:

```
header   43 4b 50 32                      CKP2
         02                               format 2
         01 e2 b7 68                      geometry id 0x68b7e201
         09 00 00 00                      step count 9
         6b 03 00 00                      body length 875
         43 4b 59 32 02 01 00 00 ...      descriptor (descriptor.bin), scheme 1
steps    01 00 00                         ERASE block 0 (it holds leftovers)
         03 00 00 00 01 40 51 d8 ...      WRITE chunk 0, count 256, then 256 data bytes
         02 01 00 01 00 c1 14 4b ...      COPY dst 1 from src 1
         02 02 00 01 00 c1 14 4b ...      COPY dst 2 from src 1 (same bytes, a different place)
         02 03 00 03 00 ...               COPY dst 3, 4, 5 from src 3, 4, 5
         03 06 00 00 01 ...               WRITE chunk 6
         03 0f 00 00 01 ...               WRITE chunk 15 (block 1 is blank: no ERASE, §7.3)
```

The vectors do not cover an unreadable manifest entry (tag `2`).

## 12. Open points

To settle with upstream before this draft is agreed:

1. **Chunk hash length**: settled (20 Oct 2026), 8 bytes (SHA-256/64) in format 2 (§2). It keeps the manifest and
   the steps small; the cost is liveness under a deliberate collision (§6.3). A longer hash would be a new format.
2. **Ed25519** as the one signature scheme, with the unsigned scheme as a named build option. ECDSA P-256 for
   parts with a hardware engine would be a new scheme number, not a change.
3. **Signing at release time**, offline, with the version and security counter fixed in the image; the server
   holds no key and only relays.
4. **Version floor and sequence numbers.** The floor is the security counter of the last confirmed image, raised
   only by the bootloader. Whether a plan or a session also needs a sequence number (replay of an old plan of a
   valid release is harmless today: it is refused below the floor, and otherwise installs a valid release).
5. **In place** (§8.2, §8.3, §9.3): the session record and apply journal formats are not fixed here yet.
