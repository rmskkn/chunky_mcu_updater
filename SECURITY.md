# Security design

What chunky trusts, who may write which part of the flash, and what each attacker can and cannot do. Code that
moves one of these lines changes this file in the same commit.

Status: designed and built on Opus (11 Oct 2026). The pass on the top model is still owed (SEC-7).

## What is protected

1. **The device runs only released firmware**, across resets, for the product it is.
2. **No rollback** to a release older than the newest security fix the device has confirmed.
3. **An update never bricks the device**: power loss at any moment, a bad image, a hostile or broken server
   and a torn flash word all end in a device that boots a verified release.
4. **The release key** signs images and nothing else, and never sits on a device, the planner or a build PC.

Not protected: confidentiality of the firmware (images and plans are plaintext), and availability against
someone who can already talk to the device (they can always refuse to send an update).

## Who is trusted with what

| Party | Trusted for | Not trusted for |
|---|---|---|
| Release signer (holds the Ed25519 private key) | What is firmware for a product, its version and security counter | — |
| Bootloader (write-protected flash, holds the public key) | Every boot decision; the only writer of ACTIVE and FLOOR | Talking to anyone: it has no transport |
| Application (the running, verified image) | Receiving a plan, writing DFU, asking for a swap, confirming itself | Deciding what is bootable: the bootloader checks again |
| Server / planner | Nothing. It holds no key | Plans, manifests it relays, blobs |
| Transport (wire_weaver over RTT, USB, radio, a modem) | Nothing | Everything it carries |
| Device manifest | Nothing. It is a hint for the planner | A wrong manifest only wastes a transfer |

The one trust anchor is the public key in the bootloader. Everything between the signer and the bootloader may
be hostile: the worst it achieves is wear and a refused update.

## The image and its descriptor

An image fills the whole ACTIVE partition: payload, `0xFF` padding, and a 128-byte descriptor at the end
(`chunky/src/descriptor.rs` has the layout). The descriptor carries the product id, the version, the security
counter, the geometry id and the SHA-256 of every byte before it. An Ed25519 signature (strict verification, a
domain string in the signed message) covers the descriptor. Nothing in ACTIVE is unsigned, padding included.

- **Product id** stops a validly signed image for another board from being installed.
- **Geometry id** stops an image built for another flash layout.
- **Version** is informational: the server compares it, the device reports it.
- **Security counter** is the anti-rollback number. It is raised only by a release that must not be rolled back
  past, so ordinary downgrades between releases with the same counter stay possible.

Per-chunk hashes are SHA-256 truncated to 16 bytes. They decide what to transfer and nothing else; whether the
result is the release is decided by the full SHA-256 under the signature.

## Where an image is checked

| When | By | What | A failure means |
|---|---|---|---|
| A plan begins | application | descriptor from the plan header: scheme, product, geometry, length, signature, floor | the plan is refused before any flash is touched |
| A plan ends | application | the whole staged image against that descriptor | no swap is requested |
| Before a swap starts | bootloader | the whole staged image again, floor included | the swap is cancelled, ACTIVE untouched |
| Every boot | bootloader | the whole of ACTIVE, floor included | a trial image is reverted on the spot; otherwise recovery, no jump |

The application's checks save wear and airtime. Only the bootloader's checks are security: it does not believe
the application.

## The version floor

A partition of its own (FLOOR) holds a unary counter: the floor is the number of write units at its start that
have been written to zero (`chunky/src/floor.rs`). It only goes up, one write unit per step, never erased.

- The **bootloader is the only writer**. It raises the floor to the security counter of the image in ACTIVE
  when that image is confirmed, never while it is on trial, so a trial that fails can still revert.
- Every check in the table above refuses an image whose counter is below the floor, on every path that picks an
  image: plan, swap, boot, revert. There is no path that boots "the other image" unchecked.
- One flipped bit in an erased unit cannot raise the floor (a unit counts when more than half its bits are
  zero), so bit rot cannot lock a device out of its own firmware.
- The partition holds `size / write unit` steps (256 on F103 and G0 presets). A counter beyond that is not
  recorded; the signing tool must refuse to issue one (SEC-3).

## Who may write which region

| Region | Bootloader | Application | A plan |
|---|---|---|---|
| Bootloader and public key | never (write-protected at provisioning) | never | never |
| STATE | swap progress, revert | "staged" and "confirmed" only, through `SwapUpdater` | never |
| FLOOR | raise | read | never |
| ACTIVE | swap and revert | read; it is handed over as `ReadOnly`, a type without a write | read (COPY sources) |
| DFU, image part | swap and revert | the apply engine | ERASE, COPY, WRITE |
| DFU, spare page | swap | never (the handle is cut short) | never |

In our code these lines are types: a piece of code can only write what it was handed. On the chip they are only
as strong as the chip's protection (next section).

## The plan is untrusted input

`chunky/src/plan.rs` and `apply.rs`. A plan can come from anyone, so:

- Fixed-size records, one encoding per plan: unknown ops, non-zero reserved bytes and stray fields are errors.
- Nothing is written before the target's descriptor verifies.
- Steps come in ascending order, an erase before the programs of its block. So each block is erased at most
  once and each chunk programmed at most once per plan, the step count is bounded by the geometry, and a COPY
  whose source was already overwritten cannot be expressed (sources are in ACTIVE, which no step writes).
- Indices are checked against the geometry; the DFU handle ends where the image ends.
- A chunk is programmed only if it reads blank, so a plan cannot make the device program a write unit twice
  (which on ECC flash would plant a faulting word).
- COPY sources and WRITE data must have the hash the step names; the chunk is read back.
- A plan made for another geometry is refused by id (`GEOMETRY_MISMATCH`) on both sides.

What a hostile plan can still do: make the device erase and rewrite DFU (wear, airtime, time) and then fail.
SEC-5 bounds that with a retry budget.

## What each attacker can do

**Someone on the network or the transport, or a hostile server.** Cannot install anything unsigned, cannot roll
back below the floor, cannot install another product's firmware. Can withhold updates, replay a real release
(refused if below the floor, harmless if not), and burn flash cycles with plans that fail (SEC-5).

**Bad luck: power loss, a flaky link, bit rot, a broken image.** The simulator cuts power at every flash
operation of a whole update on every family model and checks that the device then boots a verified release
(`chunky_sim/tests/sweep.rs`). A release that does not confirm is reverted after one trial boot. ACTIVE that
stops verifying after it was confirmed ends in recovery (no jump), not in a boot loop; falling back to the
previous image is BOOT-4.

**Code execution in the application** (a bug in the firmware, exploited). This is where the families differ:

| Family | What it can do | What it cannot do |
|---|---|---|
| **F103** | Everything, in the end. There is no RDP level 2: the application can rewrite the option bytes, lift the write protection and replace the bootloader. Signed boot on F103 protects against the network and bad luck only | — |
| **G0** | Erase or scribble ACTIVE, DFU, STATE and FLOOR: a denial of service that ends in recovery; erase FLOOR and bring back an older signed release | With RDP level 2 (option bytes frozen) and the bootloader write-protected: run anything unsigned after a reset, change the key |
| **H7** | As G0 | As G0, with RDP level 2 and secure user memory |
| **RP2350** | Scribble the other partition | Boot anything unsigned or rolled back: the boot ROM checks signatures and the OTP rollback version (BOOT-3) |

So on parts without isolation the floor holds against the network, not against code execution in the
application; and unsigned code never survives a reset wherever the bootloader itself is protected. SEC-6 sets and
checks that protection per family.

**Physical access with a debug probe.** Out of scope until SEC-6: an unprotected chip is simply reflashed.

## Building without signatures

Small-flash builds can leave the verifier out. It is a build option with a name, never a default:

- `chunky` and `chunky_swap` have features `ed25519` and `unsigned`. Neither chosen is a compile error; both on
  a firmware target is a compile error.
- An unsigned image carries the same descriptor with scheme 2 and, in place of the signature, the SHA-256 of
  the descriptor. Image SHA-256, product, geometry and the version floor are checked exactly as before.
- The scheme is inside the sealed part, and `Trust::Ed25519` refuses every other scheme: a signed device cannot
  be talked down to unsigned, and an unsigned build has no way to accept "signed" images it cannot check.
- The device says so in its manifest header; the planner returns `Warning::UnsignedDevice` for every such
  device and refuses to plan a release sealed the other way.

**What unsigned means, plainly: it protects against bad luck and broken transfers, not against anyone who can
talk to the device.** Whoever reaches the update channel can install any firmware that has the right product id
and a counter at or above the floor. Use it only where that channel is physically yours.

Flash saved by leaving the verifier out, per family: to be measured (SEC-8).

## Keys

- One Ed25519 release key per product line. The private half stays on a hardware key with an offline paper
  backup; signing is a separate step from building and planning (SEC-3). Until then the prototype signs with a
  key file the CLI creates with mode 600, and never prints it.
- Tests derive throwaway keys from fixed seeds. They are in no way secret and must never be provisioned; SEC-3
  adds a build-time marker so they cannot verify on a release build.
- Rotation (two key slots in the bootloader, a signed revocation counter) is SEC-3. Today a lost key means a
  wired reflash of every device.
- Devices have no identity key yet. Whether the planner needs an authenticated manifest at all is SEC-4: every
  COPY and WRITE is verified on the device, so a forged manifest only wastes a transfer.

## Findings so far

- **embassy-boot erases its state page while the page still says "swap".** If that erase is cut by power loss
  so that the magic survives and the progress words do not, the next boot runs swap steps again over partitions
  that are already swapped, and both images are lost. Found by the G0 sweep. `chunky_swap::Guarded` writes zeros
  over the magic before every state erase, so a torn erase reads as "boot". Reproduced stand-alone on stock
  embassy-boot 0.7.0 and git master (BOOT-5, `embassy_torn/REPORT.md`): about a quarter of all word-granular tears
  of the state erase end with a mixed ACTIVE, on the revert path and on `mark_booted` alike. Worth reporting
  upstream.
- **embassy-boot writes its "progress invalid" word a second time on some paths.** Confirmed in the BOOT-5
  model: a cut after the invalidating write and before the erase lands makes the next boot's revert write it again
  (`prepare_boot` does not check first; `set_magic` does). Zeros over zeros are
  harmless; on H7 zeros over a torn word may leave it faulting. All state reads go through `Masked`, so it reads
  as zero either way. To be confirmed on hardware (ECC-2).
- **A reverted update can read as "confirmed" rather than "reverted"** when power is lost at the very end of
  the revert. The image and the floor are right; only the report is lost. The application compares the running
  version with the one it staged (SEC-5).
