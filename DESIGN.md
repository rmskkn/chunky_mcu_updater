# Chunky Bootloader — Remote MCU Updater

Sector-delta firmware update for small microcontrollers, with an A/B partition
layout, a persisted apply journal, and a bootloader that can unroll an
incomplete update. Reference platform: **STM32F103CB** (128 KB flash, 20 KB
RAM, 1 KB flash page, 72 MHz Cortex-M3).

---

## 1. Goals and non-goals

### Goals

* The device reports a manifest of *every* flash sector it holds (`sector, md5`).
* The server computes the difference against the target image and sends only the
  sectors the device does not already have, as `sector N, blob, md5`.
* Received sectors are staged durably. The bootloader applies the staged plan,
  verifies it, and switches partitions.
* The bootloader can **unroll** a plan at any point and return to the partition
  that was running before the update started.
* A new image that boots but does not work is reverted automatically; the device
  then retries the update into the now-inactive partition.
* Interrupted transfers resume without re-sending sectors already stored.

### Non-goals

* No in-place single-partition update. The A/B layout is what makes unroll cheap.
* No compression or binary-diff inside a sector. The delta unit is one flash
  page. (Section 11 explains why.)
* No multi-generation rollback. Exactly one previous image is recoverable.
* No bootloader networking. The bootloader has no transport stack at all.
* No OTA of the bootloader itself in v1.

---

## 2. Memory map (STM32F103CB, 1 KB pages)

> Sections 2 to 8 describe the A/B layout. The host PoC implements the
> single-partition variant instead; Section 18 has the differences and the
> trade.

| Region       | Pages   | Address range             | Size  | Purpose |
|--------------|---------|---------------------------|-------|---------|
| Bootloader   | 0–14    | `0x08000000`–`0x08003BFF` | 15 KB | Apply engine, MD5, SHA-256, ECDSA sign/verify, flash driver, root public key |
| Device identity | 15   | `0x08003C00`–`0x08003FFF` | 1 KB  | Device id, device keypair, factory certificate (Section 10.2) |
| Meta bank 0  | 16–19   | `0x08004000`–`0x08004FFF` | 4 KB  | Boot state + plan (ping-pong copy A) |
| Meta bank 1  | 20–23   | `0x08005000`–`0x08005FFF` | 4 KB  | Boot state + plan (ping-pong copy B) |
| Slot A       | 24–75   | `0x08006000`–`0x08012FFF` | 52 KB | Application image |
| Slot B       | 76–127  | `0x08013000`–`0x0801FFFF` | 52 KB | Application image |

`SECTOR_SIZE` equals the flash page size and is a build-time constant: 1 KB on
medium-density F103, 2 KB on high-density parts. Slot size is 52 sectors.
Sector indices are **slot-relative** (0–51) and always travel with a slot id.

The bootloader is never erased by the update path. It is the only code that can
modify the meta banks or the inactive slot during apply.

Pages 0–15 are covered by the flash option-byte write protection (`WRP`), set at
provisioning, so neither the application nor a staged image can rewrite the
bootloader, the root public key, or the device identity.

### Meta bank layout (4 pages)

| Page | Contents |
|------|----------|
| 0    | `MetaHeader`: magic, generation, `active_slot`, `pending_slot`, `plan_state`, `target_version`, target image SHA-256, signature (64 B), `step_count`, and the per-step op/source table (52 × 2 B) |
| 1    | Target sector manifest: 52 × MD5 (16 B), indexed by slot-relative sector |
| 2    | Progress journal: 52 × 16-bit state word, advanced by clearing bits (no erase) |
| 3    | Boot attempt log: 16-bit tokens cleared one per trial boot, plus the last 8 boot outcomes |

Two banks ping-pong. The valid bank is the one with a correct CRC and the higher
`generation`. A torn write therefore never loses the previous consistent state.

F103 flash programs in 16-bit half-words and erases per page, so every field
written incrementally (journal state, attempt tokens) is a half-word whose bits
only ever go from 1 to 0:

```
0xFFFF  empty
0xFFF0  sector written to flash
0xFF00  sector written and MD5-verified
0x0000  step complete (for COPY: source read and destination verified)
```

---

## 3. Roles

| Component | Runs in | Responsibility |
|-----------|---------|----------------|
| **Server** | Cloud/LAN host | Holds every released image, its per-slot sector manifest, and a content-addressed blob store. Diffs a device manifest against a target and emits a step list. |
| **Update client** | Application firmware | Owns the transport. Computes the device manifest, receives steps, writes blob sectors into the inactive slot, fills the meta bank, reboots. |
| **Bootloader** | Pages 0–15 | Executes COPY steps, verifies the whole image, flips slots, runs trial boots, reverts, unrolls. No network. |

The host PoC in `poc/` implements exactly this split, and
[`poc/README.md`](poc/README.md) draws it: which process owns what, how the
flash regions relate, and the whole message exchange of one request.

The transport lives in the application because that is where the UART/CAN/
Ethernet/modem driver already is. Keeping it out of the bootloader is the single
biggest reliability decision in this design: the bootloader's job is small,
offline, and provable, and a bad application can never brick the device because
the recovery path does not depend on application code.

If the application is too damaged to connect, the bootloader falls back to the
previous slot (Section 7). If both slots are unusable, the bootloader stops in a
minimal recovery loop that only blinks a fault code and accepts a serial
`XMODEM`-style full-image load — no delta logic, no manifest.

---

## 4. The manifest

The device hashes **both** slots and reports all of it, as requested:

```
MANIFEST                                 // fragmented over several frames
  sector_size   : u16      (1024)
  slot_count    : u8       (2)
  sectors_slot  : u8       (52)
  active_slot   : u8
  entries[]     : { slot:u8, index:u8, md5:u8[16] }     // 104 entries

MANIFEST_END                             // one frame, closes the manifest
  manifest_sha256 : u8[32] // over the reassembled MANIFEST body above
  signature       : u8[64] // device ECDSA P-256 over the struct below
```

The body carries **no nonce**. It is a pure function of what is in flash, so
`manifest_sha256` is stable across sessions and the server can cache it, which
is what makes the elision in Section 17.1 possible. Freshness lives in the
signed structure instead, which is explicit so neither side has to agree on how
the fragments were cut:

```c
typedef struct {                /* 72 bytes, hashed then signed            */
    uint8_t  domain[8];         /* "CBMAN\0\0\0" — domain separation       */
    uint8_t  device_id[16];     /* from the identity page                  */
    uint8_t  nonce[16];         /* server's CHALLENGE                      */
    uint8_t  manifest_sha256[32];
} ManifestAuth;
```

104 entries × 18 B = 1872 B of body, fragmented across protocol frames, plus a
96-byte `MANIFEST_END`. One hash and one signature for the whole manifest —
never one per fragment, which would let an attacker splice fragments from two
sessions.

Hashing 104 KB with software MD5 at 72 MHz costs roughly 1.1 M cycles — about
15 ms. The manifest is therefore computed fresh at the start of every session
and never cached, which removes a whole class of staleness bugs.

An erased sector (all `0xFF`) still gets a real MD5. The server recognises the
constant hash of an erased page and treats it as "absent", so no special case is
needed in the wire format.

---

## 5. Diff and plan generation (server)

The server keeps, for each released version and each slot, a precomputed
manifest and the 1 KB blob behind every sector hash. The blob store is
content-addressed by MD5, so identical sectors across versions and slots are
stored once.

**Baseline for the diff.** The comparison is per destination sector, against
what the device reports *in the destination slot* — not against the running
image. The destination slot is the inactive one, so it holds whatever image was
there before (typically version `N-2`, or a partially staged `V` from an
interrupted attempt). Comparing against the destination is the only correct
baseline, because the destination is what has to end up byte-identical to `V`.

**Transfer rule.** The server sends a blob only for a destination sector whose
device-reported hash differs from the hash required by the update. Equal hash,
no bytes on the wire. That is rule 1 below, and it is the whole saving.

For target version `V` into destination slot `D`, for each destination sector
`i` with required hash `H`:

1. If the device already reports `H` at `(D, i)` → **no step**. The sector is
   already correct, which is what makes a resumed or partially-applied update
   nearly free.
2. Else if the device reports `H` anywhere — any slot, any index → emit
   **`STEP_COPY(dst=i, src_slot, src_index)`**. Zero bytes on the wire; the
   bootloader copies flash to flash.
3. Else → emit **`STEP_WRITE(dst=i, blob, md5=H)`**.

Rules 2 and 3 split the *differing* sectors by where the correct bytes can be
found. Rule 2 is a content-addressed local lookup, not a positional one: the
sector differs at the destination, but the exact bytes already sit somewhere
else in the device's flash, so the server names that location instead of
spending 1 KB of airtime. It also catches sectors that moved between versions.
Removing rule 2 would not reduce traffic — those sectors would simply become
`STEP_WRITE` and be sent over the wire.

So the wire cost of a plan is exactly:

```
bytes = SECTOR_SIZE * |{ i : target_hash(V, D, i) != device_hash(D, i)
                         and target_hash(V, D, i) not present anywhere on device }|
```

Worked example, `0x1A` → `0x1B`, 52 KB image, 10 sectors of code changed, 3 of
them pure read-only data that also exists in the active slot:

| | Sectors | Wire bytes |
|---|---|---|
| Full image | 52 | 53 248 |
| Rule 1 skip (already equal at destination) | 42 | 0 |
| Rule 2 copy (differ, found locally) | 3 | 0 |
| Rule 3 write | 7 | 7 168 |
| **Total sent** | | **7 168** (13 %) |

A retry after an interrupted attempt is cheaper still: the sectors already
staged now satisfy rule 1, so only the gap is re-sent.

The plan ends with the signed target descriptor: version, `image_sha256` over
the whole 52 KB destination slot, and an ECDSA P-256 signature over that
descriptor. Section 10.1 defines the exact hash domain.

Step ordering: the sector holding the slot header and vector table (`dst=0`) is
always emitted **last**. The slot header is the commit marker — if a transfer
dies halfway, sector 0 is still erased, the slot is unmistakably invalid, and
unroll has nothing to think about.

Section 17 quantifies what this saves, where the break-even against
sending a whole image lies, and the manifest elision that removes the fixed
manifest cost from small updates.

---

## 6. Protocol

The protocol is defined once, as messages. How those messages reach the device
is a **link adapter** (Section 16), and the message layer does not care which
one is in use.

The default adapter is a thin framing layer over a byte stream (UART, RS-485,
TCP, MQTT payloads):

```
| 0xA5 0x5A | len:u16 | type:u8 | seq:u8 | payload | crc32:u32 |
```

Payload is capped at 256 B so a 1 KB sector blob arrives as four fragments.
That keeps the client's RAM need to one sector buffer (1 KB) plus one frame
buffer (~300 B).

| Direction | Message | Payload |
|-----------|---------|---------|
| → server | `HELLO` | device id, hw rev, bootloader version, sector size, slot geometry, active slot, running version, last update result, factory certificate |
| → device | `CHALLENGE` | 16-byte server nonce |
| → server | `MANIFEST` / `MANIFEST_END` | manifest body, `manifest_sha256`, device signature (Section 4) |
| → device | `MANIFEST_REJECT` | reason (`BAD_SIGNATURE`, `BAD_NONCE`, `UNKNOWN_DEVICE`, `GEOMETRY_MISMATCH`, `MALFORMED`) |
| → device | `PLAN_BEGIN` | target version, destination slot, step count, target SHA-256, signature |
| → device | `STEP_COPY` | `dst`, `src_slot`, `src_index`, expected md5 |
| → device | `STEP_WRITE` | `dst`, `offset`, `len`, bytes, expected md5 of the full sector |
| → server | `STEP_ACK` / `STEP_NAK` | `dst`, reason (`MD5_MISMATCH`, `FLASH_FAIL`, `BAD_INDEX`) |
| → device | `PLAN_END` | — |
| → server | `PLAN_READY` | journal digest, count of steps still pending |
| → device | `COMMIT` | arm the reboot |
| → server | `STATUS` | plan state, booted version, trial result |
| → device | `ABORT` | drop the plan and unroll |

Every session is protected as described in Section 10.3, and every manifest is
signed by the device and verified by the server before a plan is generated
(Section 10.2). A session that fails either check never reaches the planner.

[`poc/README.md`](poc/README.md) has the same exchange as a sequence diagram,
drawn from the working implementation.

**Resume.** On reconnect the client repeats `HELLO`/`CHALLENGE` with the stored
`target_version`, then re-sends only the journal: a 52-entry bitmap of verified
sectors, inside the same signed `MANIFEST_END` structure. The server replays the missing steps. Nothing already in flash is sent
twice, and because every sector carries its own MD5 the device never has to
trust that a resumed stream is consistent with the earlier one.

Section 16 defines the link adapters — byte stream, Modbus, CAN with ISO-TP,
and UDS — and what each one has to provide.

---

## 7. State machine

`plan_state` in the meta header:

```
NONE ──PLAN_BEGIN──> STAGING ──all steps verified──> READY
                        │                              │
                        │                           COMMIT + reboot
                        ▼                              ▼
                     (ABORT)                        APPLYING ──> TRIAL
                        │                              │            │
                        ▼                              ▼         confirm()
                      UNROLL  <────verify fail──────────            │
                        │                                           ▼
                        └────────────> NONE (boot active)         COMMITTED
```

### Boot sequence

1. Select the valid meta bank (CRC + highest generation). If neither is valid,
   assume `active_slot = A`, `plan_state = NONE`.
2. **`plan_state == READY` or `APPLYING`** → set `APPLYING`, then for each
   journal step not yet `0x0000`:
   * `STEP_COPY`: copy `SECTOR_SIZE` bytes from `(src_slot, src_index)` to
     `(pending_slot, dst)` using a RAM-resident program routine, re-hash the
     destination, mark `0x0000`.
   * `STEP_WRITE`: the application already wrote it; re-verify its MD5 and mark
     `0x0000`. A mismatch means the staged data is bad → **unroll**.
3. Verify the full pending image against `image_sha256` and the ECDSA
   signature. Failure → **unroll**.
4. Set `pending_slot` to trial: write the attempt log with `MAX_TRIES = 3`
   tokens, set `plan_state = TRIAL`, bump generation.
5. **`plan_state == TRIAL`** → clear one attempt token. If none remain →
   **revert** (Section 8). Otherwise start the independent watchdog (IWDG,
   ~8 s), set `SCB->VTOR` to the pending slot base, and jump.
6. **`plan_state` otherwise** → recompute `image_sha256` over `active_slot` and
   compare it with the slot descriptor. Match → boot. Mismatch → the slot has
   rotted; **revert** to the other slot if its descriptor verifies, otherwise
   enter the recovery loop. The signature is *not* re-checked here (Section 10.1).

### Application duties on a trial boot

The new image must reach a working state — transport up, peripherals probed,
server reachable — and then call `cb_confirm()`, which sets
`active_slot = pending_slot`, `plan_state = COMMITTED`, clears the attempt log
and disables the watchdog escalation. A hang, a crash, or a hard fault instead
produces a watchdog or fault reset, which costs one attempt token. Three bad
attempts revert.

The old slot is left untouched and becomes the inactive slot — and therefore the
destination of the *next* update.

---

## 8. Unroll and revert

Two distinct operations. Both are cheap because of one invariant:

> **Apply invariant.** From `PLAN_BEGIN` until `cb_confirm()`, nothing outside
> the pending slot, the meta banks, and the journal is ever written. The active
> slot is read-only for the entire update.

**Unroll** — abandon a plan that has not booted yet (verify failed, `ABORT`,
staged MD5 mismatch, signature mismatch):

1. Erase sector 0 of the pending slot. The slot header disappears, so the slot
   is unambiguously invalid.
2. Write a new meta generation: `plan_state = NONE`, `pending_slot = NONE`,
   attempt log cleared, `active_slot` unchanged, and a `last_result` code the
   server will read in `HELLO`.
3. Boot `active_slot`.

No inverse journal is needed; because of the invariant there is nothing to
inverse. Unroll is O(1) — one page erase and one meta write — and it is itself
idempotent, so a power cut during unroll just means unroll runs again.

**Revert** — a committed-but-broken image, after `MAX_TRIES` failed trial boots:

1. Mark the pending slot header `REVOKED` so it is never retried.
2. Write a new meta generation: `active_slot` unchanged (it was never moved),
   `plan_state = NONE`, `last_result = TRIAL_FAILED(version)`.
3. Boot the active slot, which is the image that was running before the update.
4. The application reports `TRIAL_FAILED` in `HELLO`. The server can mark that
   version bad for this device class and plan a different target into the
   revoked slot, which is now simply the inactive slot again.

---

## 9. Failure matrix

| Failure point | Result |
|---|---|
| Power loss mid-download | Journal survives; resume sends only unverified sectors |
| Power loss mid flash program | That sector's journal word is not `0xFF00`; it is re-sent or re-copied |
| Power loss during apply | `plan_state == APPLYING`; the bootloader resumes the journal from where it stopped |
| Power loss during meta write | The other bank with the lower generation is still consistent |
| Corrupt blob in transit | Per-sector MD5 fails → `STEP_NAK` → re-sent |
| Server sends a wrong target | `image_sha256` or release signature fails → unroll |
| TLS handshake fails or the pin does not match | No session; link fault reported, retry with backoff |
| Manifest signature or nonce is wrong | `MANIFEST_REJECT`; no plan is generated |
| Device identity page blank or `factory_cert` bad | Device cannot sign; reports a provisioning fault and does not update |
| New image hard-faults immediately | Fault handler resets; attempt tokens run out; revert |
| New image hangs | IWDG resets; attempt tokens run out; revert |
| New image works but cannot reach the server | No `cb_confirm()` → revert. Deliberate: unreachable is indistinguishable from broken |
| Both slots invalid | Bootloader recovery loop, serial full-image load |
| Flash wears out on a sector | Program/erase verify fails → `FLASH_FAIL` → unroll, reported to the server |

---

## 10. Security

MD5 is used **only** for per-sector equality and transport integrity, exactly as
specified. It is not a trust boundary: a collision would let an attacker who
already controls the server substitute a sector.

Authenticity comes from one signature per image, checked by the bootloader
before the slot is ever made bootable:

* The public key lives in the bootloader region, which the update path cannot
  write.
* `micro-ecc` verify costs roughly 200–400 ms at 72 MHz — paid once, at commit.
* `version` is monotonic and the bootloader refuses a target whose version is
  lower than the running one, unless the plan carries an explicit
  `allow_downgrade` flag that is itself inside the signed descriptor.

### 10.1 Image descriptor and hash domain

A whole-image hash cannot be stored naively inside the image it covers, so the
domain is defined with a hole.

The slot begins with the vector table, because `SCB->VTOR` points at the slot
base. The descriptor sits immediately after it at a fixed offset:

| Slot offset | Size | Contents |
|---|---|---|
| `0x000` | 512 B | Vector table (`VTOR` needs the table 512-byte aligned at this size) |
| `0x200` | 128 B | `ImageDescriptor` |
| `0x280` | … | Code and read-only data, `0xFF` padding to the slot end |

```c
typedef struct {                  /* slot_base + 0x200, 128 bytes      */
    uint32_t magic;               /* 'CBIM'                            */
    uint32_t version;             /* monotonic                         */
    uint32_t image_len;           /* always SLOT_SIZE (53248)          */
    uint32_t flags;               /* bit0 allow_downgrade              */
    uint8_t  image_sha256[32];    /* over the domain below             */
    uint8_t  signature[64];       /* ECDSA P-256 over bytes [0, 0x30)  */
    uint32_t reserved[2];
} ImageDescriptor;                /* 0x30 signed prefix + sig + pad    */
```

**`image_sha256` domain:** slot bytes `[0x000, 0x200)` then `[0x280, 0x0D000)`
— the whole slot, with the 128-byte descriptor excluded. The vector table *is*
covered, and so is every padding byte, so the hash is defined even for a
partially used slot. No circularity: the descriptor covers the image, and the
signature covers the descriptor's first `0x30` bytes, which include
`image_sha256`.

**Signature domain:** `ImageDescriptor` bytes `[0x00, 0x30)` only — magic,
version, length, flags, and `image_sha256`. Nothing else needs signing, because
`image_sha256` already commits to the entire image.

**Where it is checked.**

| When | Check | Cost |
|---|---|---|
| Server, on `PLAN_BEGIN` | descriptor is relayed to the device | — |
| Bootloader, at commit (Section 7 step 3) | `image_sha256` **and** signature | ~18 ms + ~300 ms, once per update |
| Bootloader, on every normal boot | `image_sha256` only | ~18 ms per boot |

SHA-256 over 52 KB costs about 1.3 M cycles, roughly 18 ms at 72 MHz. That is
cheap enough to pay on every boot, and it is worth paying: it turns flash bit
rot in a long-deployed image into a clean revert instead of an undefined crash,
and the attempt-token path then recovers the device by itself. The signature is
checked only at commit — re-checking it on every boot costs 300 ms and proves
nothing new, since a slot's bytes cannot change without `image_sha256` changing.

**It does not replace the per-sector hashes.** The whole-image hash is a
pass/fail gate; it cannot say *which* sector differs, so there is no delta
without the per-sector manifest. The two hashes answer different questions:
per-sector MD5 answers "what must I transfer", `image_sha256` answers "is the
result exactly the released image". The design needs both.

**Optional: drop MD5 entirely.** Per-sector hashes could be SHA-256 truncated
to 16 bytes instead, keeping the manifest the same 1872 B and removing the weak
primitive from the device altogether. Cost: manifest generation goes from ~15 ms
to ~35 ms for 104 KB, and the bootloader drops the MD5 implementation (~1.5 KB)
in exchange for reusing SHA-256 it already carries — a net flash *saving*. The
wire format stays `sector, hash, blob`. MD5 is kept here only because it was
specified; if the `sector,md5` format is not an external constraint, truncated
SHA-256 is the better default.

### 10.2 Manifest authentication

Every manifest is signed by the device and verified by the server before the
planner runs. An unsigned or unverifiable manifest produces `MANIFEST_REJECT`
and the session ends with no plan.

**Device identity** lives in page 15, write-protected by `WRP` and provisioned
at manufacture:

```c
typedef struct {                  /* 0x08003C00, 1 KB page            */
    uint32_t magic;               /* 'CBID'                           */
    uint8_t  device_id[16];
    uint8_t  priv_key[32];        /* ECDSA P-256 private scalar       */
    uint8_t  pub_key[64];         /* uncompressed, X || Y             */
    uint8_t  factory_cert[64];    /* factory CA signature over
                                     device_id || pub_key             */
    uint8_t  reserved[...];       /* 0xFF                             */
} DeviceIdentity;
```

`factory_cert` means the server needs only the factory CA public key, not a
pre-registered row per device: a device can present itself on first contact and
be trusted for its own updates.

**Deterministic signatures, RFC 6979.** The STM32F103 has **no hardware RNG**.
A conventional ECDSA signature with a weak or repeated per-signature nonce `k`
leaks the private key outright, so signing uses `uECC_sign_deterministic()` with
HMAC-SHA256, where `k` is derived from the private key and the message hash. No
entropy source is required and the failure mode disappears. Cost: roughly
250 ms at 72 MHz, once per session.

**Freshness.** The server sends a 16-byte `CHALLENGE` nonce before the manifest,
and the nonce is inside `ManifestAuth` (Section 4). A manifest captured from an
earlier session therefore cannot be replayed to make the server plan against
stale flash contents. The server keeps the nonce for the lifetime of the session
only and rejects any manifest whose nonce does not match the one it issued.

**Server-side checks, in order.** All of them must pass before planning:

1. `factory_cert` verifies against the factory CA public key over
   `device_id || pub_key`, and `device_id` is not on the revocation list.
2. `manifest_sha256` matches the SHA-256 of the reassembled manifest body.
3. `signature` verifies against `pub_key` over `ManifestAuth`, with the
   `domain` field exactly `"CBMAN"`-padded — domain separation prevents a
   signature made for one message type from being replayed as another.
4. `nonce` equals the nonce issued in this session.
5. Structure: `sector_size` and slot geometry match what is registered for this
   `device_id`'s hardware revision; exactly `slot_count × sectors_slot` entries;
   every `(slot, index)` present exactly once; no index out of range.
6. `active_slot` is a valid slot and is consistent with the `HELLO` report.

Check 5 is not ceremony. A manifest with a duplicated or missing `(slot, index)`
would make the planner emit a plan that does not cover the destination slot, and
the device would then stage an image that fails `image_sha256` — a wasted
transfer that is far cheaper to reject up front.

**Why sign at all when the link is TLS.** TLS authenticates the *connection*;
the signature authenticates the *statement*. They fail differently, and on this
reference platform the difference is concrete: TLS is terminated in a network
module (Section 10.3), so the module sits inside the transport trust path. The
device signature is what makes that acceptable — a compromised module, a
terminating proxy, or a logging gateway can read and reorder the stream but
cannot forge a manifest, and cannot make the server generate a plan for flash
contents that do not exist. It also gives the server a non-repudiable record of
what each device actually reported.

**Key compromise.** Flash readout protection on the F103 (`RDP` level 1, or
level 2 in production, which permanently disables the debug port) raises the
cost of extracting `priv_key` but does not defeat a determined attacker with
physical access. That is accepted: the device key proves *which device is
speaking*, it does not protect firmware confidentiality, and the blast radius of
one extracted key is exactly one device lying about its own flash contents. It
cannot be used to sign firmware — images are signed by the release key, whose
public half is in the bootloader (Section 10). Revoke the `device_id` and the
key is worthless.

**Application reads the key.** The signing code runs in the application, so a
sufficiently broken application could leak `priv_key`. `WRP` stops it being
*modified*, not read. Accepted for the same reason as above; moving signing into
the bootloader would require the bootloader to be resident and own the
transport, which is the one thing Section 3 deliberately refuses.

### 10.3 Transport

**TLS is mandatory on every IP hop.** No plaintext fallback exists in the
protocol; a device or gateway that cannot establish TLS reports a link fault and
retries with backoff. Where the device sits behind a gateway on a field bus
(Section 16), TLS covers server-to-gateway, and the bus segment is covered by
the end-to-end signatures instead — the same split as the MCU-to-module UART on
the reference platform. If a bus segment also needs confidentiality, wrap the
message payloads in an AEAD under a per-device pre-shared key; the device
already has a provisioned identity to derive it from. That is the only case
where the design adds a second cipher, and it is not needed for integrity or
authenticity, only for secrecy on the wire.

* TLS 1.3, ECDHE with AES-128-GCM or ChaCha20-Poly1305.
* The server is authenticated against a **pinned** root public key held in the
  bootloader region, not against a device trust store. Hostname verification on.
* Client certificates are *not* required: the manifest signature already
  authenticates the device, end to end, and adding mTLS would duplicate that at
  the cost of per-device key material inside the TLS module. Enable mTLS only
  where a network policy demands it at the connection layer.

**The F103 does not terminate TLS itself.** This is a hard platform limit, not a
preference. A minimal mbedTLS TLS 1.3 client with one cipher suite and P-256 is
roughly 30–45 KB of flash; with `MBEDTLS_SSL_MAX_FRAGMENT_LENGTH` cut to 512
and tuned I/O buffers it still wants 8–10 KB of RAM. The budget here is a 52 KB
application slot and 20 KB of RAM total, shared with the update client, the
sector buffer, and the actual product firmware. It does not fit, and pretending
otherwise would be the kind of thing that fits in a spreadsheet and not in the
part.

So on the reference platform TLS is **offloaded to the network module** — an
ESP32 running ESP-AT, or an LTE modem such as a BG95/SIM7600 with its built-in
SSL stack. Requirements on that module, whatever the AT syntax:

* The CA or pinned key is provisioned into the module, certificate **and**
  hostname verification are enabled, and any "insecure" or "verify none" mode is
  disabled.
* Plaintext fallback is disabled, so a downgrade attempt fails closed.
* The module's own firmware is updated through its vendor path; it is in the
  trust path for confidentiality, which is exactly why Section 10.2 signs the
  manifest.

The MCU-to-module hop is a plain UART and is **not** covered by TLS. That is why
the framing layer keeps its `crc32` (Section 6) — it protects the local hop,
where TLS does not reach.

**If TLS must terminate on the MCU**, the F103CB is the wrong part. Use an
STM32L4/F4 with ≥ 256 KB flash and ≥ 64 KB RAM, or an STM32L5/H5 with on-die
AES and PKA, where the handshake is both affordable and faster. Nothing else in
this design changes: the partition layout, journal, and plan format are
independent of the transport.

**What stops what.**

| Attack | Stopped by |
|---|---|
| Passive capture of the session | TLS |
| Active MITM on the network | TLS with a pinned root key |
| Plaintext or cipher downgrade | No fallback path in the protocol or module config |
| Replay of a captured manifest | `CHALLENGE` nonce inside the signature |
| Forged manifest from a compromised gateway or module | Device manifest signature |
| Cross-message signature reuse | `domain` field in `ManifestAuth` |
| Forged or substituted firmware image | Release-key signature over `ImageDescriptor` (Section 10.1) |
| Rollback to an old vulnerable image | Monotonic `version`, enforced by the bootloader |
| Corruption on the MCU-to-module UART | `crc32` in the framing layer |

---

## 11. Decisions and tradeoffs

**Sector-granular delta, not byte-granular.** The flash page is the smallest
unit that can be erased and rewritten, so a byte-level diff would still force a
full page rewrite, and it would need a scratch buffer to hold the old page while
reconstructing the new one. Page-granular delta needs no reconstruction state at
all: a sector is either correct or it is overwritten. The cost is that a one-byte
code change still transfers 1 KB. For a 52 KB image a typical change touches
15–30 % of sectors, so the saving over a full image is already 3–6×.

**Blobs are written straight to their final address.** There is no separate
staging region, because the inactive slot *is* scratch space by definition. This
is what makes 52 KB slots possible in 128 KB of flash. The journal, not a
staging copy, provides crash safety.

**Two per-slot builds, not one position-independent build.** The application is
linked twice, once per slot base, and the server holds a manifest for each. GCC's
ROPI/RWPI support for Cortex-M3 is too weak to rely on, and relocating at boot
would double the flash writes. The cost is that code sectors differ between
slots, so cross-slot `STEP_COPY` only hits sectors without absolute references —
constants, tables, and read-only data. Those copies are still free, and the
in-slot rule 1 ("already correct") is where most of the saving comes from on a
resume or a re-update.

**COPY runs in the bootloader, WRITE runs in the application.** Copies are the
only steps that need no network, so putting them in the bootloader means the
apply phase can always complete offline and resume after any reset. It also
matches the requirement that the plan is applied by the bootloader. The
application only ever writes sectors it received, immediately, straight from the
frame buffer.

**TLS is offloaded, and the manifest is signed anyway.** Terminating TLS in the
network module is the only option that fits a 52 KB application slot, but it
puts the module inside the transport trust path. Rather than accept that, the
device signs its manifest with its own key, so the strongest claim the module
can make — "this is what the device reports" — is one it cannot forge. The cost
is ~3 KB of flash, ~250 ms per session, and a provisioning step at manufacture.
The alternative, trusting the module, would mean a single compromised radio
firmware could make the server plan updates against fiction.

**Confirm is explicit, and silence means revert.** A device that boots but
cannot talk to the server is operationally identical to a bricked one, so it is
treated as a failed trial. This makes "cannot reach the server after an update"
self-healing rather than a field visit.

---

## 12. Resource budget

| Item | Size |
|---|---|
| Bootloader code (flash driver, MD5, SHA-256, micro-ecc, journal, apply) | ~11–14 KB of the 15 KB region |
| Device identity page | 1 KB (page 15, `WRP` protected) |
| Update client in the application | ~3–4 KB, plus micro-ecc sign + HMAC-SHA256 (~3 KB), plus the AT driver |
| Manifest signing time | ~250 ms, once per session |
| TLS stack on the MCU | 0 — terminated in the network module |
| Application RAM: sector buffer | 1024 B |
| Application RAM: frame buffer | ~300 B |
| MD5 context | 92 B |
| SHA-256 context | 108 B |
| Manifest on the wire | 1872 B |

---

## 13. Server side

* **Image registry.** For each `(version, slot)`: the 52-entry sector manifest,
  the full image SHA-256, and the signature. Produced at build time by a
  `mkmanifest` tool that splits the ELF-derived binary on page boundaries and
  pads the tail with `0xFF`.
* **Blob store.** Content-addressed by MD5, one 1 KB object per unique sector.
* **Device table.** Per device: last `HELLO`, running version, active slot,
  current plan, journal bitmap, last result, and the set of versions known bad
  for that device.
* **Planner.** Stateless given a manifest and a target: the three rules of
  Section 5. Steps are emitted `dst=0` last.
* **Rollout control.** Target version per device group, a cap on concurrent
  updates, and automatic suspension of a rollout once `TRIAL_FAILED` crosses a
  threshold — a bad image reverts on each device by itself, but it should stop
  reaching new ones.

---

## 14. Build order

1. Flash driver with a RAM-resident program/erase routine; MD5; the meta bank
   ping-pong with CRC and generation.
2. Bootloader boot path: bank selection, slot header validation, `VTOR` jump.
   No plans yet.
3. `mkmanifest`, the blob store, and the planner, with a host-side simulator
   that stands in for the device.
4. Framing layer, `HELLO`/`MANIFEST`, and `STEP_WRITE` on the device.
5. `STEP_COPY` and the apply journal in the bootloader.
6. Trial boot, attempt tokens, `cb_confirm()`, revert.
7. Provisioning: identity page writer, factory CA, `WRP`/`RDP` option bytes.
8. TLS in the network module with a pinned root key, plaintext fallback removed.
9. `CHALLENGE`, deterministic manifest signing, and the six server-side checks.
10. Release-key signature verification and version monotonicity.
11. Fault injection: cut power during download, during apply, and during the
    meta write; corrupt a staged sector; ship an image that hard-faults, and one
    that hangs; replay a stale signed manifest; present a bad server certificate;
    flip a bit on the MCU-to-module UART.

---

## 15. Note: integration with MCUboot

**Verdict: possible, and a good fit — in `direct-xip` mode, with an unmodified
MCUboot.** The delta layer in this document sits entirely *above* a bootloader;
MCUboot can be that bootloader. Roughly half of the device-side state invented
here then disappears, replaced by MCUboot's image header, TLVs, and trailers.
The cost is flash: MCUboot is several times larger than the 15 KB bootloader
budgeted in Section 2, which is uncomfortable on a 128 KB F103CB.

MCUboot has no delta or differential update support upstream. Its upgrade modes
move whole images — `swap-using-scratch`, `swap-using-move`,
`swap-using-offset`, `overwrite-only`, `direct-xip`, `ram-load`. Nothing in
MCUboot competes with the manifest/plan protocol; the two parts compose rather
than overlap.

### 15.1 Why `direct-xip`

MCUboot's documented limitation — images are "built to run from a fixed
location (i.e., not position-independent)", and `IMAGE_F_PIC` is "Not
supported" — is the same constraint that led to two per-slot builds in
Section 11. `direct-xip` executes in place from whichever slot holds the image,
with no copy and no swap, which matches this design's A/B layout exactly:

* No swap means no scratch area, no sector shuffling, and no swap status.
* Rollback is slot selection, not data movement — the same O(1) property the
  apply invariant in Section 8 was built to get.
* `MCUBOOT_DIRECT_XIP_REVERT` adds the trial-boot/confirm cycle, which is this
  design's trial boot.

One real behavioural difference: `direct-xip` selects **the slot holding the
newest valid image by version number**, not a stored `active_slot` pointer. The
pointer in the meta bank goes away, and `version` becomes load-bearing rather
than advisory. Release versions must therefore be strictly monotonic per
device, which this design already requires (Section 10).

### 15.2 Mapping

| This design | MCUboot equivalent | Notes |
|---|---|---|
| Slot A / Slot B | Primary / secondary slot via `flash_map` | Same geometry |
| `ImageDescriptor` at `0x200` (Section 10.1) | `struct image_header` at slot offset 0, code at `ih_hdr_size` | Drop ours entirely |
| `image_sha256` | `IMAGE_TLV_SHA256` (`0x10`) over header and body | Drop ours |
| Release ECDSA P-256 signature | `IMAGE_TLV_ECDSA_SIG` + `IMAGE_TLV_KEYHASH` | Drop ours; key is built into MCUboot |
| Meta bank ping-pong (8 KB, Section 2) | Image trailer per slot: magic, `image_ok`, `copy_done` | **Deleted** |
| `plan_state`, `pending_slot`, `active_slot` | Trailer flags plus version-based slot selection | **Deleted** |
| Apply journal, `STEP_COPY` execution in bootloader | None — nothing to apply in `direct-xip` | **Deleted**, see 15.4 |
| Trial boot + `cb_confirm()` | `MCUBOOT_DIRECT_XIP_REVERT` + `boot_set_confirmed()` | Direct map, with one caveat (15.5) |
| Attempt tokens, `MAX_TRIES = 3` | Single test boot | **Reduced**, see 15.5 |
| Version monotonicity | `MCUBOOT_DOWNGRADE_PREVENTION`, or `MCUBOOT_HW_ROLLBACK_PROT` with a security counter in the protected TLV (`imgtool -s`) | Prefer the security counter |
| Manifest, `CHALLENGE`, plan, `STEP_*` (Sections 4–6) | **No equivalent** | Keep as-is, app-side |
| Manifest signing, TLS (Sections 10.2–10.3) | **No equivalent** | Keep as-is, app-side |

### 15.3 Revised memory map (F103CB, 1 KB pages)

| Region | Pages | Size | Purpose |
|---|---|---|---|
| MCUboot | 0–30 | 31 KB | Bootloader with SHA-256 + ECDSA P-256 |
| Device identity | 31 | 1 KB | Section 10.2, `WRP` protected |
| Slot 0 | 32–79 | 48 KB | 47 image sectors + 1 trailer sector |
| Slot 1 | 80–127 | 48 KB | 47 image sectors + 1 trailer sector |

The 31 KB figure must be measured, not assumed: an MCUboot build with SHA-256
and ECDSA P-256 is typically in the 30–40 KB range, and if it lands above 31 KB
the slots shrink further. At 48 KB slots the manifest covers 47 sectors per slot,
94 entries, 1692 B.

This is tight. **For a new design that integrates MCUboot, specify an F103RE
(256 KB) or larger.** The layout, protocol, and planner are unchanged by the
larger part; only the numbers move. On the CB the choice is genuinely between
MCUboot's maturity and ~16 KB of application flash.

### 15.4 What changes in the delta layer

**`STEP_COPY` moves into the application.** MCUboot will not perform flash-to-
flash copies on our behalf, so the application executes both step types: read
the source sector from the other slot, write it to the destination. No harder
than `STEP_WRITE` — the application already runs its program routine from RAM
because an F103 stalls the bus during a flash write. Rule 2 of Section 5 and its
zero-airtime benefit survive intact.

**The apply journal becomes unnecessary.** It existed to let the bootloader
resume a half-applied plan. With `direct-xip` there is no apply phase, and
progress is already recorded in the one place that cannot lie: the destination
slot's own contents. On reconnect the device re-hashes and re-sends its
manifest, the server re-runs the same diff, and the gap is exactly the set of
sectors still wrong. **Resume falls out of the design rather than being
implemented** — the 52-entry bitmap of Section 6 can be dropped, and with it
one meta page and a class of journal/flash disagreement bugs.

**The trailer sector is never diffed.** Its contents change as a result of
booting — `image_ok`, `copy_done` — so a trailer sector included in the manifest
would appear permanently different and would be overwritten by a blob,
destroying boot state. Rule: the manifest covers image sectors only; the trailer
sector is written unconditionally as the **last** step of every plan.

**The trailer replaces sector 0 as the commit marker.** MCUboot does not
consider an image valid without a trailer — the documentation is explicit that
without one "MCUboot will not recognize the image as valid and will attempt to
revert it". So an interrupted transfer leaves a trailerless slot that MCUboot
ignores, and unroll becomes *doing nothing at all*. The Section 5 rule of
emitting `dst=0` last is replaced by "trailer last".

**Images must be signed artifacts, and the manifest must be built from them.**
`mkmanifest` runs on the output of `imgtool sign --pad`, not on the raw binary,
so the sector hashes match what actually lands in flash byte for byte. `--pad`
makes the padding and trailer deterministic, which is what makes the hashes
stable across builds of the same image.

### 15.5 Gaps to close

1. **Standalone port.** MCUboot's non-Zephyr path needs `flash_area_*` /
   `flash_map` for our layout, an `os`/`sysflash` shim, and a boot application
   that jumps to the chosen slot. This is the bulk of the integration work and
   it is well-trodden, but it is not free.
2. **One test boot, not three.** `direct-xip` revert gives a single trial: if
   the image does not confirm before the next reset, it is reverted. The
   `MAX_TRIES = 3` attempt-token log of Section 7 has no MCUboot equivalent.
   Either accept one attempt — simpler, and arguably correct, since an image
   that fails its self-test once is unlikely to pass the third time — or keep a
   retry counter in application-owned flash and only call
   `boot_set_confirmed()` once the counter is exhausted or the self-test passes.
   Accepting one attempt is the recommendation; it deletes the attempt log, the
   boot-log page, and the token bookkeeping.
3. **`image_sha256` on every boot.** Section 10.1 pays ~18 ms per boot to catch
   bit rot. MCUboot validates the image hash and signature on boot according to
   its configuration; confirm the chosen configuration actually hashes on every
   boot rather than only on upgrade, and adjust the expectation rather than
   adding a second hash pass.
4. **`BOOT_MAX_IMG_SECTORS`** defaults to 128 and sizes internal state. With
   48-sector slots, lower it to reduce RAM.
5. **Per-device state has no home.** MCUboot owns the trailers; the device
   identity page (Section 10.2) and any application-owned retry counter need
   their own reserved pages, excluded from both slots and from the manifest.

### 15.6 When not to integrate

If the product is pinned to an F103CB and needs the application flash, the
15 KB purpose-built bootloader in Sections 2–8 is the smaller answer: it does
only `direct-xip`-style A/B with revert, which is the only mode this design
uses, and it costs roughly half of what MCUboot costs to get there. If the part
has room, or the project already uses Zephyr, MCUboot is the better default —
the trailer, TLV, and revert machinery here would otherwise be a
reimplementation of something that is audited, fuzzed, and widely deployed.

Either way the manifest protocol, the planner, the device manifest signature,
and TLS are unaffected. They are the part of this design MCUboot does not have.

---

## 16. Transport

A device on a field bus is reached through a **gateway**: the server speaks TLS
to the gateway over IP, and the gateway relays the same messages onto the bus.
This is not a new trust model — Section 10.3 already accepts a non-TLS last hop,
because on the reference platform TLS terminates in the network module and the
MCU-to-module UART is unprotected. A Modbus or CAN segment is the same shape
with a longer last hop, and it is safe for the same reason: the manifest
signature and the release signature are end to end, so a gateway can read and
reorder traffic but cannot forge what the device reports or what it installs.

An adapter must provide four things, and nothing more:

1. Deliver one message, in order, to one addressed device.
2. Declare its maximum payload per transaction.
3. Declare whether it already provides per-transaction integrity and
   sequencing.
4. Declare which side initiates.

Point 3 earns its place: on a link that already checksums and sequences every
transaction, the framing `crc32` is redundant and is omitted, saving 4 B per
frame. Reassembly errors are still caught, because the per-sector hash is the
only end-to-end integrity check that actually matters.

Point 4 matters more than it looks. The message layer is request/response in
both directions, but a polled bus has no device-initiated path at all, so on
those adapters the gateway drives every exchange and the device only answers.

| Adapter | Max payload | Frames per 1 KB sector | Integrity | Initiator |
|---|---|---|---|---|
| TCP + TLS | 256 B (RAM-bound, not link-bound) | 4 | TLS AEAD | either |
| UART / RS-485 | 256 B | 4 | framing `crc32` | either |
| Modbus RTU / TCP | 246 B (FC 16, 123 registers) | 5 | Modbus CRC16 / TCP | master only |
| CAN 2.0 + ISO-TP | 4095 B per message | 147 CAN frames | CAN CRC + ISO-TP sequence | either |
| CAN FD + ISO-TP | 4095 B per message | 18 CAN FD frames | CAN CRC + ISO-TP sequence | either |

### 16.1 Modbus

The device is a slave and exposes a mailbox in its holding-register space: a
data window, a command register, and a status register. The master writes a
chunk with `Write Multiple Registers` (FC 16, 123 registers = 246 B maximum per
transaction), then writes the command register to commit it. A 1 KB sector is
five transactions, roughly 5 % ADU overhead — cheaper than the byte-stream
framing it replaces.

The flow inverts. Modbus has no server push, so the device cannot announce
itself; instead it raises an "update wanted" bit in its status register, the
master polls for it, and the master then **reads** the manifest out of the
device with `Read Holding Registers` rather than receiving it. 1872 B of
manifest is 8 reads. Everything else is unchanged, because the message layer was
already request/response.

Modbus is also where the round-trip cost bites hardest: it is strictly one
transaction at a time, so a 1 KB sector is five round trips. At 19200 baud a
sector takes about 1.5 s, which makes a 7-sector delta ~11 s against ~80 s for
a full image. This is the transport where delta pays for itself most obviously,
and where the manifest elision of Section 17.1 is worth the most.

`Read/Write File Record` (FC 20/21) is the more semantically honest mapping and
is a reasonable alternative, but register mailboxes are what field tooling and
existing Modbus masters actually support without special cases.

### 16.2 CAN

Use **ISO-TP (ISO 15765-2)** for segmentation: a 1 KB sector is one multi-frame
message, with a per-device RX/TX identifier pair for addressing. On classic CAN
at 500 kbit/s a sector is 147 frames and about 33 ms; CAN FD cuts that to 18
frames. Bandwidth is not the constraint on CAN — **coexistence is**.

Two rules keep an update from disturbing the machine it is running on:

* Give update traffic a **high numeric CAN identifier**, so arbitration makes
  control and safety frames win automatically. This is free and it is the whole
  reason CAN prioritisation exists.
* Cap bus utilisation with ISO-TP `STmin`, sized so update traffic stays under a
  budget (20 % is a reasonable default) rather than filling whatever is idle.

**If the product already speaks UDS, reuse it.** `RequestDownload` (0x34),
`TransferData` (0x36) and `RequestTransferExit` (0x37) carry a delta plan
directly: the sector index goes in the address field of `RequestDownload`, one
`TransferData` sequence per sector. The manifest is read with `ReadDataByIdentifier`
(0x22) against a dedicated DID, or `ReadMemoryByAddress` (0x23). That reuses an
audited, already-certified stack instead of adding a second protocol to the same
bus, and it keeps diagnostic tooling working. The planner, the manifest, and
both signatures are unaffected — only the framing changes.

### 16.3 Very low rate links

On LoRaWAN or a similar sub-kbit/s link the manifest alone is minutes of
airtime. Manifest elision (Section 17.1) stops being an optimisation and becomes
a requirement, and the ACK window should be the whole plan: one ACK bitmap at
`PLAN_END`, never per sector.

---

## 17. Overhead analysis

Delta is not free. It adds a manifest, a signature, and two round trips to
every session, and those costs are paid whether one sector changed or all of
them did. The honest question is where the crossover sits.

Framing costs 10 B per frame (2 magic + 2 length + 1 type + 1 sequence +
4 CRC), and each sector fragment also carries a 5-byte step header, so at a
256 B data chunk a 1024 B sector costs 1024 + 4×15 = 1084 B, plus a
`STEP_WRITE_END` and a `STEP_ACK`: **1123 B measured**. Fixed session cost is
`HELLO` + `CHALLENGE` + manifest (1872 B of body) + `MANIFEST_END` +
`PLAN_BEGIN` + the closing messages: **2328 B measured**.

The numbers below are **measured**, not modelled: they come from the host PoC
in `poc/`, which runs the real protocol over a real socket against a 128 KB
emulated flash and a 45 772 B reference binary. The PoC prints the same
figures, one row per update request, for whatever release it is given.

Steady state, destination slot holding the previous release:

| Sectors changed | Delta on the wire | Fraction of a full image |
|---|---|---|
| 2 | 4 574 B | 7.5 % |
| 5 | 7 943 B | 13.1 % |
| 12 | 15 804 B | 26.0 % |
| 26 | 31 526 B | 51.9 % |
| 52 (everything) | 60 724 B | 100.0 % |

The full-image baseline is 60 724 B: the same protocol with every sector forced
to a write. Three things in that table are worth more than the ratios.

**Delta is never worse, and the worst case is a tie.** When every sector has
changed, a delta plan is exactly a full transfer plus the manifest, and the
manifest is itself elided or amortised. The earlier estimate in this document
put break-even at 50 of 52 sectors and the worst case at 103.7 %; that assumed
the full transfer was a raw stream with no per-sector protocol of its own. Any
real full-image transfer needs its own framing, acknowledgements and integrity
checks, so measured against a like-for-like baseline there is no crossover at
all. The design therefore needs no "send it all instead" escape hatch.

**A two-sector change costs 4 574 B**, of which 2 048 B is payload: the fixed
manifest cost dominates small updates. With a warm server-side manifest cache
(Section 17.1) the same update measures **2 498 B, 4.1 %** — the elision is
worth 2 076 B, and more than a third of the session.

**Rule 2 earns nothing in steady state and a great deal on a first update.**
Measured with the destination holding the previous release, turning rule 2 off
changes nothing at all (13.1 % either way): the sectors that differ there are
genuinely new bytes that exist nowhere on the device. But updating into an
**erased** slot, rule 2 finds 40 of 45 sectors in the active slot and drops the
session from 87.1 % to 15.8 %. That is where content-addressed local copies pay,
and it is also the case a naive implementation gets worst.

**Sector-delta efficiency is hostage to code layout.** This is the real risk,
and it is not visible in the table. Inserting one function near the start of
the image shifts every byte after it, so every following sector's hash changes
and a one-line fix turns into a near-full transfer. Mitigations, in order of
value:

* Build reproducibly — same toolchain, same flags, sorted link order, no
  timestamps or paths baked into the binary. Without this the whole scheme
  degrades toward full transfers for reasons unrelated to the actual change.
* Compile with `-ffunction-sections -fdata-sections` and keep an explicit,
  stable symbol order in the linker script, so unrelated edits do not permute
  layout.
* Place volatile, frequently-tuned data (calibration tables, strings, limits)
  in their own sector-aligned section at the **end** of the image, so changing
  them cannot shift code.
* Accept the rest. Code churn genuinely spread across the image is a genuinely
  large update, and no sector-granular scheme can fix that.

**Against a compressed full image.** LZ-class compression on ARM Thumb code
reaches roughly 60–70 % of the original, so a compressed full transfer is
around 35 KB — better than delta once churn exceeds about 60 % of sectors, and
much worse below it. Compressing each 1 KB blob independently gains little,
because a 1 KB window has almost no history to exploit; compressing the blob
*stream* compresses well but couples the sectors together and destroys
per-sector resume. The two techniques are complementary rather than
alternatives: if the link is expensive enough to justify both, compress each
blob with a shared pre-trained dictionary, which keeps sectors independent and
recovers most of the ratio. Not in v1.

**Round trips, not just bytes.** On a high-latency link — NB-IoT with a 1–2 s
RTT, or a polled field bus — the ACK-per-sector pattern dominates the transfer
time. Delta already wins here (7 round trips instead of 52), but the protocol
should allow a window of several outstanding sectors and a single ACK bitmap at
`PLAN_END` rather than one ACK per sector. With a window of 8 the 7-sector case
becomes 2 round trips.

**Cryptography is cheap; flash programming is not.** The cryptography is
roughly what it looks like: manifest hashing ~15 ms, manifest signing ~250 ms,
`image_sha256` ~18 ms, release-signature verify ~300 ms. Under 0.6 s, and
invisible next to a transfer that takes ~10 s at 9600 baud.

Writing the flash is the part that is easy to forget. Datasheet DS5319
Table 23, "Flash memory characteristics", gives for this part:

| Symbol | Parameter | Min | Max | Unit |
|---|---|---|---|---|
| `tprog` | Word programming time | 20 | 40 | µs |
| `tERASE` | Page (1 kB) erase time | 20 | 40 | ms |
| `tME` | Mass erase time | 20 | 40 | ms |

The **Typ column of that table is empty**: ST publishes a minimum and a
maximum and no typical, so every device-time figure here is a range, and a
schedule should be built on the maximum. The part programs 16 bits per
operation, so one 1 KB page is one erase plus 512 programming operations:

* one sector: **30.2 ms best case, 60.5 ms worst case**
* a full 52 KB slot: **1.57 s to 3.14 s**

Endurance is the other half of the same budget: 1 kcycle minimum in DS5319
Rev 1, 10 kcycles in later revisions. Anything rewritten on every boot must
therefore clear bits rather than erase a page, which is what the journal
encoding and the attempt-token log in Section 2 are for.

Measured from the PoC, which counts the operations and models the device time:

| Attempt | Device time | of which flash |
|---|---|---|
| 2-sector update, whole session | 417–569 ms | 151–303 ms |
| 5-sector update, whole session | 509–751 ms | 242–484 ms |
| 12-sector update, whole session | 723–1177 ms | 454–908 ms |
| Bootloader apply plus verify | 410–504 ms | 91–182 ms |
| Each further trial boot | 91–182 ms | 91–182 ms |
| Revert after the last trial | 220–422 ms | 202–403 ms |
| Ordinary boot, nothing pending | 18 ms | 0 ms |

### Savings, both currencies

Measured end to end for a 5-sector change in steady state, against the same
protocol sending every sector:

| | Delta | Full image | Saved |
|---|---|---|---|
| Wire | 7 943 B | 60 724 B | **52 781 B, 86.9 %** |
| Sectors written | 5 | 52 | 47 |
| Flash time | 151–302 ms | 1 572–3 145 ms | **1.4–2.8 s** |
| Link at 9600 baud | 8.3 s | 63.2 s | 54.9 s |
| Link at 115200 baud | 0.7 s | 5.3 s | 4.6 s |
| Total at 9600 baud | 8.4–8.6 s | 64.8–66.4 s | **56.2–57.8 s** |
| Total at 115200 baud | 0.8–1.0 s | 6.8–8.4 s | **6.0–7.4 s** |

**Copies save bytes but not flash time, and that changes where rule 2 is
worth having.** A `STEP_COPY` still erases and programs the destination page,
so it costs the device exactly what a `STEP_WRITE` costs: the only thing it
saves is airtime. Measured on a first update into an erased slot, rule 2 takes
the wire cost from 87.1 % to 15.8 % of a full image while the flash cost stays
at 1.36–2.72 s either way. Two consequences:

* On a slow or metered link, rule 2 is close to free money.
* On a fast link, rule 2 buys almost nothing, because flash programming is the
  floor. What matters there is rule 1 — the sector that is **already correct**
  and so is neither sent nor written.

Rule 1 is the only rule that saves both currencies, and it is also the rule
that makes resume cheap, which is the strongest argument for diffing against
the destination slot rather than against the running image.

### 17.1 Manifest elision

The manifest is a flat 1.9 KB per session, and on a metered or very slow link
that is the dominant cost of a small update. It is also redundant: the server
already knows what the device's flash contains, because the server is the only
thing that ever changes it.

So the device may send a digest instead of the full manifest:

```
MANIFEST_DIGEST
  nonce         : u8[16]     // from CHALLENGE, so this is not replayable
  manifest_sha256 : u8[32]   // over the canonical manifest body
  signature     : u8[64]     // same ManifestAuth struct as Section 4
```

The server compares the digest with the manifest it has cached for this
`device_id`. A match means its cached copy is byte-identical and planning
proceeds immediately. A mismatch — first contact, a cache eviction, a device
whose flash was changed by a debugger or a factory tool — gets
`MANIFEST_NEEDED`, and the device sends the full manifest as in Section 4. The
canonical body is defined bit-for-bit (Section 4) precisely so this digest is
reproducible on both sides.

Measured: the fixed session cost drops by 2 076 B, which moves the two-sector
case from 4 574 B to **2 498 B**, 4.1 % of a full image, and the five-sector
case from 7 943 B to 5 867 B. The saving is flat, so it matters most exactly
where it should: on the smallest updates.

The digest is signed and carries the nonce for the same reason the manifest
does: it is a statement about device state, and the server must not plan against
an unauthenticated or replayed one.

---

## 18. Single-partition variant

Everything above assumes A/B: two application partitions, the inactive one
rewritten and the active one untouched, which is what makes unroll O(1) and
revert possible at all. The host PoC in `poc/` implements a **different
layout**, chosen deliberately: one application partition, updated in place.

| | A/B (Sections 2-8) | Single partition (`poc/`) |
|---|---|---|
| Application space | 2 x 52 KB | 104 KB |
| Sectors in the manifest | 104, both slots | 104, the one partition |
| Manifest body | 2088 B | 1876 B |
| Rule 1, already correct | fires on every update | fires on every update |
| Rule 2, local copy | finds sectors in the other slot | almost never fires |
| Unroll | one page erase | not possible |
| Trial boot and revert | yes, one generation back | not possible |
| Resume after a power cut | yes | yes |
| Who owns the transport | the application | **the bootloader** |

Three consequences are worth stating plainly, because they are not obvious
from "drop the second slot".

**The running image cannot do the writing.** An in-place update overwrites the
code that is executing, so the download and the programming have to move into
the bootloader. That reverses the decision in Section 3, and it is the largest
structural difference between the two layouts: the bootloader now needs a
transport, which is exactly the surface Section 3 was keeping out of it.

**A failed update is not a failed update, it is a device with no firmware.**
There is no previous image to fall back to, so the recovery path is the update
itself: the bootloader reports that it has no bootable image and asks for
another release. The device is recoverable as long as the server is reachable,
and bricked if it is not. Rollback is traded for 52 KB of application space and
nothing else.

**Resume survives, rollback does not.** The partition's own sector hashes are
the progress record, so a power cut mid-update costs only the sectors not yet
written: the next request re-manifests and the server re-diffs. That is the
same property as Section 15.4, and it is what keeps in-place updating viable
at all.

**Rule 2 stops paying.** A copy needs its bytes to already exist in the flash
the manifest covers. With one partition that means elsewhere in the same image,
which for real firmware is almost never true -- measured on the PoC, both
requests in its demo copy nothing. The content-addressed lookup of Section 5
is cheap enough to keep, but it is A/B that makes it valuable, because the
other slot holds a related release.

Measured on the PoC, 104 KB partition, 45 772 B reference binary:

```
case                       skip copy write     wire of full   saved B   saved      flash ms
-------------------------- ---- ---- ----- -------- ------- --------- ------- -------------
v2                           99    0     5     7984    6.7%    111870   93.3%       151-302
v3                           89    0    15    19284   16.1%    100570   83.9%       454-907
```

The ratios look better than Section 17's only because the image is twice as
large while the change is the same size: a fixed-size change against a bigger
image is a smaller fraction of it. The absolute numbers are what matter, and
they are unchanged -- 5 changed sectors still cost about 8 KB and 151-302 ms.

Choose A/B when a failed update must not strand the device. Choose one
partition when the application needs the space and the link is reliable enough
that "ask again" is an acceptable recovery path.
