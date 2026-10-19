# chunky_bootloader vs chunky_mcu_updater

As of 2026-10-19. Compared against
[romixlab/chunky_mcu_updater](https://github.com/romixlab/chunky_mcu_updater)
`main`: `SPEC.md` (format 2, draft PROTO-1), `SECURITY.md` and
`chunky_format/c/chunky_format.h`. Their `DESIGN.md` and `README.md` are an
older copy of ours (MD5 era, "design only") and are not compared. "Ours" means
our [`DESIGN.md`](../DESIGN.md) plus the PoC in this repository.

chunky_mcu_updater is a Rust rewrite of the same sector-delta idea. It has a
stricter signed descriptor, a persistent anti-rollback floor and an apply engine
in the application. Our PoC has compression and an in-place mode that it lacks.
It also avoids one gap that our PoC has: our PoC writes flash before it checks
the plan's signature.

## Main differences

Seven differences shape the two designs. Everything else in the side-by-side
table follows from them.

1. **Who applies the update.** Theirs: the running app applies every step into
   DFU, and the bootloader only verifies and switches. Ours: COPY runs in the
   bootloader with a journal (DESIGN), or the bootloader owns the transport and
   updates in place (PoC).
2. **Trust model.** Theirs trusts one thing: the release key in the
   bootloader. The manifest, server and transport are untrusted, and there is
   no TLS. Ours adds a device identity, a signed manifest with a server nonce,
   and mandatory TLS.
3. **What the release key signs.** Theirs: the whole 64 B descriptor body with
   a domain string, which binds product, geometry and security counter. Ours:
   a 48 B prefix with version and flags only. It has no domain string and no
   product or geometry.
4. **Anti-rollback.** Theirs: a stored floor that only goes up, separate from
   the version. Ours: a monotonic version with a signed `allow_downgrade` flag.
   Nothing is stored.
5. **Flash model.** Theirs: a chunk can be smaller than the erase unit,
   explicit ERASE, program only if blank, and BLANK and UNREADABLE manifest
   entries for ECC parts. Ours: a sector of whole erase units, implicit erase,
   and no handling of ECC faults.
6. **Wire efficiency.** Theirs: one raw chunk per WRITE and a fixed 8 B hash.
   Ours: LZ4 (also with the old sector as dictionary), manifest elision, and a
   hash width of 8..32 B.
7. **Progress record.** Theirs and our PoC: flash is the record, and the
   device re-manifests after any break. Our A/B DESIGN: meta banks and a
   journal.

## What chunky_format.h is

A C reader that shrink_wrap's `head_tail` generator (SW-46) produces from the
Rust types in `chunky_format`. It has no structs: each field is an `*_OFFSET`
macro plus a `static inline` little-endian getter, so it is safe on unaligned
bytes and needs no packing. Fixed fields form the head. Variable data forms the
tail, which `sw_ht_take` walks with a bounds check.

| Record | Size | Fields |
| --- | --- | --- |
| Scheme | 1 B | tag: 1 Ed25519, 2 unsigned |
| DescriptorBody | 64 B | magic `CKY2`, format, scheme, reserved[2], product_id, version, security_counter, image_len, geometry_id, image_sha256[32], reserved[4] |
| SignedDescriptor | 128 B | body + 64 B seal |
| PlanHeader | 145 B | magic `CKP2`, format, geometry_id, step_count, body_len, SignedDescriptor at offset 17 |
| Step | 3, 13 or 13 + chunk B | ERASE{block u16}, COPY{dst u16, src u16, hash[8]}, WRITE{dst u16, count u16, hash[8]} + data |
| ManifestHeader | 13 B | magic `CKM2`, format, backend, scheme, geometry_id, chunks u16 |
| ChunkEntry | 9 or 1 B | HASH{hash[8]}, BLANK, UNREADABLE |

The flow is as follows. The device sends a manifest with one entry per chunk,
ACTIVE first and then DFU. The server answers with a plan of ERASE, COPY and
WRITE steps, headed by the target image's signed descriptor. The device checks
every step, then the whole image.

## Side by side

| Topic | chunky_mcu_updater | chunky_bootloader |
| --- | --- | --- |
| Who applies the plan | The running app applies ERASE, COPY and WRITE into DFU. The bootloader only verifies and switches (page swap, `SWAP_BANK`, or RP2350 boot ROM) | DESIGN: COPY in the bootloader, with a journal. PoC: in place, and the bootloader owns the transport (§18) |
| Progress record | None in A/B: re-manifest and re-plan after any break (their §9.1) | A/B: meta banks, journal, plan states. PoC: flash is the record, as in theirs |
| Descriptor location | Last 128 B of the slot. The hash covers `[0, slot_len-128)` | At `0x200`, after the vector table. The hash range has a hole |
| Commit marker | Descriptor chunk, last because steps go in ascending order | Sector 0, sent last |
| Signature | Ed25519 `verify_strict` over a domain string + the whole 64 B body. Reserved bytes must be zero | ECDSA P-256 over bytes `[0, 0x30)`. No domain string. `reserved` is not signed |
| Fields in the signed descriptor | product_id, geometry_id, security_counter, version | version, flags (`allow_downgrade`). No product or geometry |
| Anti-rollback | FLOOR partition: unary counter, raised by the bootloader only on confirm | version must not go below the running one, unless the signed `allow_downgrade` flag is set. Nothing is stored |
| When the seal is checked | Plan begin (before any flash write), end of plan, before every swap, every boot | Bootloader at commit. Normal boots check the hash only |
| Manifest trust | Untrusted and unsigned. No device key (SEC-4 open) | Device ECDSA signature, factory cert, server nonce (§10.2) |
| Manifest entries | HASH 9 B, BLANK 1 B, UNREADABLE 1 B (ECC fault after a torn write) | A hash per sector, erased sectors included. No notion of ECC faults |
| Delta unit | Chunk can be smaller than the erase unit (their vectors: 256 B chunks on 2 KB pages). Explicit ERASE step | Sector of one or more erase units, needed for journal atomicity |
| Plan rules | Strictly ascending position. Program only if blank. WRITE = exactly one chunk | Sector 0 last. No other ordering rule |
| Compression | None | LZ4, also with the old sector as dictionary (§17.2). Manifest elision (§17.1) |
| Sector hash | SHA-256 truncated to 8 B, fixed (their open point 1) | SHA-256 truncated to 8..32 B, set at build time, checked at `HELLO` |
| Transport | WireWeaver `ww_dfu`. Trusted for nothing. No confidentiality | Own framing. TLS mandatory on IP hops. Challenge nonce |
| Testing | Golden vectors shared by the Rust and C readers. Power-cut simulator at every flash op. Hardware bench on F103, G0, H7, RP2350 | Host PoC on mmap'd file flash. `case_*.sh` integration tests |

## Gap in our PoC: flash is written before the signature is checked

`lib/device.c:137-156` accepts `PLAN_BEGIN`, copies `image_sha256` and
`image_mac` into the pending meta, and starts to take WRITE steps. The MAC is
checked only at boot (`device.c:411`). The PoC updates in place, so anyone who
can reach the session can overwrite the running image without any key. The
device then has no bootable firmware until a good update arrives.

Their SPEC §6.1 forbids this. No flash is touched until the descriptor seal
passes. Cheap checks that need no key (magic, format, bounds, reserved bytes,
length) run before the seal, so junk never costs a signature verification.

Fix: verify the descriptor MAC at `PLAN_BEGIN`, before the first
`stage_sector`. Add a `case_*.sh` test that sends a plan with a bad MAC and
checks that the partition is not changed.

## Items for consolidation

The goal is one format that both projects can share. Take their layer 0
(descriptor, plan, manifest, receiver checks) as the base, add our wire savings
to it, and settle three open decisions. Each row names the side to converge on.

| # | Item | Converge on | What changes | Who changes |
| --- | --- | --- | --- | --- |
| 1 | Seal checked before the first flash write, cheap refusals first | Theirs (SPEC §4.3, §6.1) | Verify the descriptor at `PLAN_BEGIN` in `lib/device.c` | Us |
| 2 | Descriptor: last 128 B of the slot, 64 B body + 64 B seal, domain string, reserved bytes zero | Theirs (SPEC §4) | Drop the `0x200` placement and the hole in the hashed range (DESIGN §10.1, `lib/image.h`) | Us |
| 3 | Signed fields: product_id, geometry_id, security_counter | Theirs | Add them to `image_desc_t`. The device checks product and geometry, not only `HELLO` | Us |
| 4 | Anti-rollback: stored floor raised on confirm, separate from version | Theirs (SECURITY.md) | Replace `allow_downgrade`. Add a FLOOR region to `device.toml` | Us |
| 5 | Signature scheme byte in the descriptor | Theirs | Ed25519 as scheme 1. ECDSA P-256 (micro-ecc) only as a new scheme number | Us |
| 6 | Geometry id: one u32 hash over the geometry | Theirs (FNV-1a 32, SPEC §2) | `gen_device.py` emits it. `HELLO` compares it in place of separate fields | Us |
| 7 | Plan steps ERASE/COPY/WRITE, ascending order, program only if blank | Theirs (SPEC §5, §6.2) | Explicit ERASE. The device checks ordering and blank | Us |
| 8 | BLANK and UNREADABLE manifest entries | Theirs (SPEC §7.2) | Needed before any G0 or H7 port | Us |
| 9 | Progress: flash is the record, re-manifest after a break | Theirs, already in our PoC | Retire the A/B journal and meta-bank plan in DESIGN §2 and §7 | Us (DESIGN only) |
| 10 | WRITE encoding: raw, LZ4, LZ4 with old-sector dictionary | Ours (DESIGN §17.2) | Add an encoding byte to WRITE. `count` becomes the encoded length. The hash stays over the decoded chunk | Them |
| 11 | Manifest elision by cached digest | Ours (DESIGN §17.1) | Optional carrier feature, layer 1 | Them |
| 12 | In-place delivery | Merge: their spec (§8.2, §8.3, §9.3), our PoC as first implementation | Our §18 maps onto their `1L`. Fix item 1 first | Both |
| 13 | Layering: layer 0 octets, layer 1 delivery, layer 2 carrier | Theirs (SPEC §1) | Our frame format becomes one carrier, next to `ww_dfu` | Us |
| 14 | Shared golden vectors | Theirs (SPEC §11) | PoC tests read and apply their `chunky_format/vectors` | Us |

Open decisions, to settle before the format is frozen:

- [ ] Sector hash width: fixed 8 B (theirs) or configurable 8..32 B and
  checked at session start (ours). A fixed width per format version keeps one
  encoding per value.
- [ ] Manifest authentication: none (theirs) or device-signed with a nonce
  (ours: about 3 KB of flash, about 250 ms per session, and a provisioning
  step).
- [ ] Transport security: TLS required on IP hops (ours), or out of scope for
  the format (theirs).

## Notes on the header

These come from reading only. Nothing was compiled or run.

- `sw_ht_take` computes `(size_t)(end - at)`. If a caller passes `*t > end`,
  the value wraps to a huge number and the bounds check passes. The function
  never advances past `end` itself, so only a broken caller can trigger this. A
  guard on `at > end` would close it.
- `chunky_step_tail` returns -1 after it has already advanced `*t` and set
  `tl->head`, so a caller must discard the cursor on error. `tl->write_data` is
  set only for WRITE.
- The magic values and the format byte are not emitted as constants. C code
  must hardcode `CKY2`, `CKP2`, `CKM2` and `2`.
- Tag numbering differs: ChunkEntry starts at 0, Step and Scheme start at 1.
- `sw_fixed_bits` is undefined for `n = 0` (shift by 64). Nothing in this
  header calls it.
- Their `SECURITY.md` reports a torn-erase bug in embassy-boot that can mix
  ACTIVE and DFU (BOOT-5). Read it before any port based on embassy-boot.
