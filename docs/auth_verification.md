# Authenticating a plan as it arrives, and at boot

As of 2026-10-19. Design proposal, not implemented. Starting point: the gap
flagged in [`compare_chunky_mcu_updater.md`](compare_chunky_mcu_updater.md)
("Gap in our PoC: flash is written before the signature is checked") —
`lib/device.c:137-156` accepts `PLAN_BEGIN`, copies `image_sha256` and
`image_mac` into the pending meta, and starts taking WRITE steps. The MAC is
checked only at boot (`device.c:411`). Anyone who can reach the session can
overwrite the running image without any key.

> **Update, 2026-10-20 (A/B migration landed, `docs/ab_migration.md`):** the
> specific gap above is closed, exactly the way the next section predicted —
> not by adding a check, but because writes now land in the pending slot,
> which the apply invariant (`DESIGN.md` section 8) keeps safe from the
> active one, and nothing boots the pending slot until the bootloader's own
> verify-then-trial-boot sequence (sections 7-8) passes. An unauthenticated
> session can still fill the inactive slot with garbage; it can no longer
> make garbage bootable. Everything below that is framed as "once A/B
> lands" is now the current state of the PoC, not a future one. The schemes
> and open items past the gap analysis — per-sector-table authentication,
> meta header integrity — are unaffected by partition layout and remain
> exactly as open as before; the gap analysis itself is kept as written
> because the argument (why the gap was in-place-specific, not an A/B flaw)
> is the useful part, not a prediction that still needed to come true.

## The current scheme

`image_mac` (`lib/image.c:40-43`) is `HMAC(root_key, magic || version ||
image_len || flags || image_sha256)` — a MAC over the *target descriptor*,
where `image_sha256` is `SHA-256` over the whole 52 KB image
(`lib/flash_view.c:30-34`). That hash cannot be computed until every sector
has arrived, so today nothing authenticates the plan until the image is
complete and the device has already rebooted into it (`lib/device.c:380-410`).
Per-chunk, `stage_sector()` only checks a chunk against the plan's own
declared per-sector hash — correctness ("did this chunk decode to what the
plan claims"), not authenticity ("did a key-holder sign this"). A session
with no key can send a self-consistent plan (hashes matching its own
payloads) and every chunk "passes."

This matches DESIGN.md §10.1's own split: per-sector hashes decide what to
transfer and are not a trust boundary; the whole-image `SHA-256` under the
signature decides whether the result is the release. The split is sound —
the bug is that the signature side of it is checked too late, after the
write already happened.

## This gap is specific to the in-place variant, not a flaw in A/B

Writing the pending slot early is not, by itself, the problem — in A/B
(DESIGN.md §2-8) that is exactly what the design intends. The apply
invariant (DESIGN.md §8) is explicit: "From `PLAN_BEGIN` until
`cb_confirm()`, nothing outside the pending slot, the meta banks, and the
journal is ever written. The active slot is read-only for the entire
update." Writing an unverified image into the *inactive* slot carries no
risk, because nothing boots it yet.

The invariant that actually matters is narrower, and A/B already has it:
**the bootloader is the only thing that moves `active_slot`, and only after
its own verification passes.** The updater (application) writes steps, fills
the meta bank, and sets `pending_slot` — a candidate, not a commitment
(DESIGN.md §3: "writes blob sectors into the inactive slot, fills the meta
bank, reboots"). `pending_slot` is only ever promoted to `active_slot` by
`cb_confirm()` (DESIGN.md §7, "Application duties on a trial boot"), and
`cb_confirm()` only runs *inside* a trial boot that the bootloader already
jumped into — which it only does after its own `image_sha256`/signature
check passed (DESIGN.md §7 boot sequence, steps 3-4: verify, then set
`plan_state = TRIAL`). So by construction, nothing boots an image the
bootloader has not already verified, and `active_slot` cannot point at one.
There is no equivalent gap to close in A/B.

**The gap is real only because the PoC (DESIGN.md §18) has no second slot.**
With one partition, "the pending thing" and "the thing that boots next" are
the same flash — there is no `active_slot` index to gate, because there is
no inactive slot to stage into first. An authenticated session is the only
thing standing between "accept a WRITE" and "that WRITE is now what boots
next." That is exactly why Scheme A (verify the plan's hash table, or at
minimum `image_mac`, before accepting the first step) matters here and
would be redundant in A/B: in-place has no bootloader-held veto point after
the write, so the veto has to move before it.

## Two schemes, not one

| | Scheme A: chunk arrival | Scheme B: whole image, bootloader |
|---|---|---|
| Covers | `{dst_i, hash_i}` table, dst-bound, ascending order | Whole slot, raw bytes, descriptor hole excluded |
| Width | `SECTOR_HASH_LEN` (8 B default) — fine | Full `SHA256_LEN` (32 B), never truncated |
| Signed when | Once per plan, sent upfront (`PLAN_BEGIN` or a new `PLAN_TABLE`) | Once per build, baked into the descriptor by the signer |
| Checked when | Once per plan (signature), then per chunk (table lookup, no crypto) | Every boot (cheap recompute + compare); signature only at build/commit |
| Stops | A bad or substituted chunk, rejected the moment it arrives | Whatever scheme A's truncation missed; also bit rot, unrelated to any update |
| Lives in | Session state (`meta_t`, transient, this plan only) | `image_desc_t`, persisted in flash, forever |

Scheme A is a liveness optimization layered on top of scheme B, which stays
the actual security boundary. A forged chunk that happens to collide scheme
A's truncated hash still has to pass scheme B's full 32 B check at the end,
which it cannot without breaking `SHA-256` outright — so a collision at
scheme A only buys an attacker "caught at commit/next boot instead of this
instant," the same failure mode that exists today, not a new one. That is
also why scheme B must use full-width hashes if it moves to the fold
mechanism below: it cannot borrow A's width without losing the
defense-in-depth that makes A's cheap width safe to begin with.

## Scheme A: an authenticated per-sector hash table

**Why a bare concatenation of hashes is not enough.** `H = hash_0 | hash_1 |
... | hash_{N-1}` lets two different `{dst, hash}` assignments fold to the
same `H` if hashes are relabeled to different destinations in an
order-preserving way. A buggy server, or a MITM who can reorder or retarget
steps without forging a new signature, could swap sector 5's hash onto
sector 12's slot and vice versa — same fold, same valid signature, wrong
image. The fold must bind position:

```
H = SHA256(dst_0 || hash_0 || dst_1 || hash_1 || ... || dst_{N-1} || hash_{N-1})
plan_mac = HMAC(root_key, version || image_sha256 || H)   // ECDSA in production
```

Steps must also arrive in one canonical order (ascending `dst`) so both
sides fold the same way — a rule we do not currently enforce
(chunky_mcu_updater's SPEC §5.4 does: "steps go up through the slot").

**Getting the authentication before the first chunk, not after the last.**
`plan_mac` can only be checked once every `(dst, hash)` pair it covers is
known. If those pairs keep streaming in per-step as they do today, the
earliest point `H` can be verified is after the *last* step — no better than
checking `image_mac` at `PLAN_BEGIN` as it stands. The win only appears if
the server sends the whole `{dst, hash}[N]` table upfront, signed as one
unit, before any `STEP_COPY`/`STEP_WRITE`:

- A new message pair, shaped like the existing `MANIFEST`/`MANIFEST_END`
  fragmenting: `PLAN_TABLE` carries fragments of `(dst, hash)` pairs,
  `PLAN_TABLE_END` carries `plan_mac`. Table size is
  `(2 + SECTOR_HASH_LEN) × N` — about 1 KB for 102 sectors at 8 B, the same
  order as the manifest body.
- The device verifies `plan_mac` once, before accepting a single step.
- Each `stage_sector()` call then checks the arriving chunk's hash against
  an *already-authenticated* table entry instead of a self-declared one, so
  a bad or substituted chunk is rejected the instant it arrives, not merely
  caught eventually.
- `meta_t` gains a "table verified" flag that gates `stage_sector()`; a plan
  cannot reach `S_PLAN` without it set.

## Scheme B: the bootloader's whole-image check, reusing the same fold

The boot-time check (DESIGN.md §7 step 6) can stay exactly as it is —
`image_hash_flash()` over the raw slot bytes, descriptor hole excluded,
compared against the descriptor's stored `image_sha256`. That already works
and needs no change to close the reported gap.

Reusing scheme A's fold mechanism for this check too (one hashing code path
instead of two) is a legitimate simplification, but three things have to
hold or it is a regression, not a win:

1. **Width.** The fold must use full `SHA256_LEN` (32 B) per-sector hashes,
   computed separately from the wire-truncated `SECTOR_HASH_LEN` ones.
   Folding the manifest's truncated hashes here would drop the boot-time
   check's collision resistance from 2^128+ to whatever `SECTOR_HASH_LEN`
   gives (2^32 at the 8 B default) — a real loss versus today's full
   `SHA-256`.
2. **The descriptor's own sector needs the same hole `image_hash()` already
   carves out.** `image_desc_t` sits at `DESC_OFF = 0x200`, inside whichever
   sector spans that offset (sector 0 at the default 1 KB geometry). That
   sector's raw bytes include the descriptor's own `sha256`/`mac` fields;
   folding its full raw bytes in makes the fold depend on its own stored
   result. That one sector's hash-for-folding must exclude
   `[DESC_OFF, CODE_OFF)`, the same 128 B `image_hash()` already excludes.
3. **The trusted fold must live in the descriptor, not in `meta_t`.**
   `meta_t`'s `target_sha256`/`target_mac` are per-plan and reset after a
   cycle. The boot-time reference has to persist across every future boot,
   independent of any one update — so `H` is written into `image_desc_t` at
   `image_build()` time (replacing or sitting beside `sha256[32]`), signed
   once there. Every later boot recomputes `H` fresh and compares; no
   signature re-check per boot, same cost split as today (cheap hash every
   boot, expensive signature only at build/commit).

Net cost once those three hold: about the same total bytes hashed (the same
~52 KB, as `N` separate `SHA-256` passes instead of one continuous one —
negligible difference), for one hashing mechanism instead of two.

## Meta header integrity

Schemes A and B authenticate what comes *over the wire*. `meta_t`
(`lib/flash.h:107-121`) is a third surface: it is written to flash by the
device itself, mid-session, and read back across resets, reboots and the
bootloader's apply loop. Today it carries only a CRC-32
(`lib/flash.c:132-156`, `crc32_buf(m, sizeof *m)`), and the valid bank is
picked by "correct CRC and higher `generation`." CRC-32 catches accidental
corruption — a torn write, a bit flip from flash wear — with very high
probability. It is linear and not keyed, so it catches nothing deliberate: a
party able to write specific bytes into the meta bank region directly
(bypassing `device_session_feed`/`device_boot` entirely — a fault-injection
or glitching attack, or any other channel that writes flash bytes without
going through the normal code path) can set `plan_state = COMMITTED`,
`target_sha256`/`target_mac`, or (once Scheme A lands) the per-sector
`target_hash` table and the "table verified" flag to anything, then
recompute a matching CRC-32 over the tampered bytes trivially. The ping-pong
generation counter does not help either — it only says which of two banks
is newer, not whether either one's content is honest.

**What a keyed MAC does and does not fix.** Replace the CRC-32 with
`HMAC(key, m without crc/mac field)`, same primitive already in the design.
This closes the "write flash bytes directly, skip the code path" attacker
described above — they would also need the key. It does **not** help
against a compromised application (DESIGN.md §10's own scoping: on F103,
"code execution in the application" already gets "everything, in the end,"
listed as an accepted risk, not something signed boot defends against). If
the key used is `dev_key`, a compromised application already has legitimate
runtime access to it (DESIGN.md §10.2: "a sufficiently broken application
could leak `priv_key`"), so it could forge a valid MAC over any `meta_t`
content it likes — no improvement over today's CRC-32 for that specific
attacker. The two threats are genuinely different attackers, and a `dev_key`
MAC only closes the narrower one (direct flash tampering without code
execution), which is still worth doing: same hash primitive, no new key
material, no change to the wire protocol.

**The deeper fix, if the compromised-application case also matters:** keep
the application from ever being the thing that *asserts* `plan_state =
READY`/`COMMITTED` or writes the trusted target hashes at all. DESIGN.md
§11 already draws this line for the data path — "COPY runs in the
bootloader, WRITE runs in the application" — because only the bootloader's
code is required to be small, offline and provable (§3). The same split
applied to `meta_t` means: the application may write unauthenticated
bookkeeping (which sectors it has staged so far, bytes received) into a
non-security-relevant region, but the security-relevant transition —
"this plan's signature checked out, mark it `READY`" — is made only by
bootloader-callable code, using a key the application never holds. That
key cannot be `dev_key` (application already has it) or `root_key`
(real production holds only the *public* half on-device; nothing on-device
can mint a new valid signature with it) — it has to be a third, local,
bootloader-only secret, provisioned the same way `dev_key`/`root_key` are
(DESIGN.md §10.2's `DeviceIdentity` page) but never exposed outside
bootloader code. This is a bigger structural change than Scheme A/B: it
means the apply/commit decision — not just the COPY step — moves into the
bootloader's exclusive territory, consistent with where the design already
puts every other security-relevant check (DESIGN.md's "Where an image is
checked" table: "Only the bootloader's checks are security: it does not
believe the application.").

**Scoping for this PoC.** The host PoC merges application-side session
handling and bootloader-side apply/boot into one `device.c` for the host
demo, so there is no in-process boundary today to put a bootloader-only key
behind. The practical next step here is the cheap one — swap the CRC-32 for
a `dev_key`-HMAC over `meta_t`, closing the direct-tampering case — and
leave the bootloader/application key split as a note for whenever the PoC
or a real port gives the two roles separate code and separate key access.

## Open items

- [ ] Decide whether scheme B keeps today's raw-image `image_sha256` as is
  (simplest, already works, zero risk) or moves to the full-width fold
  (unifies the code path, needs the three conditions above).
- [ ] Protocol: add `PLAN_TABLE`/`PLAN_TABLE_END`, or fold the table into an
  extended `PLAN_BEGIN` if it fits the frame size budget (Section 6).
- [ ] Enforce ascending `dst` order on the device for both the manifest-diff
  steps and the new table, matching chunky_mcu_updater's SPEC §5.4.
- [ ] Thread `root_key` into `device_session_t`/`device_session_start()` —
  today only `device_boot()` receives it (`lib/device.h`).
- [ ] `meta_t`: add the "table verified" gate and, if scheme B changes,
  the full-width per-sector hash cache needed to recompute `H` without
  re-reading flash sector by sector twice.
- [ ] Test: a `PLAN_TABLE_END` with a bad `plan_mac` is rejected before any
  `stage_sector()` call, and flash is unchanged.
- [ ] Test: a step whose `(dst, hash)` is not in the verified table is
  rejected, even if its data's own computed hash is internally consistent.
- [ ] Replace `meta_t`'s CRC-32 (`lib/flash.c:132-156`) with a `dev_key`-HMAC
  over the same range, closing the direct-flash-tampering case described
  above. Bump `meta_t`'s magic alongside the layout change.
- [ ] Decide whether the bootloader/application key split for `meta_t`'s
  security-relevant fields (the deeper fix, above) is worth doing in this
  design, or stays accepted risk alongside the rest of DESIGN.md §10's
  compromised-application scoping.
- [ ] Test: a `meta_t` bank with a correct CRC-32 (today) or correct MAC
  (after the fix) but a `plan_state`/`target_hash` that was never produced
  by `device_session_feed`/`device_boot` is rejected — write the test
  before the fix to show it currently passes when it should not, then after
  to show it is caught.
