# chunky_mcu_updater

Remote firmware updater for small microcontrollers that transfers **only the
flash sectors that actually changed**, and that can always get the device back
to the firmware it was running before the update started.

Reference platform: **STM32F103CB** — 128 KB flash, 20 KB RAM, 1 KB flash page,
72 MHz Cortex-M3.

> **Status: design only.** No code yet. [`DESIGN.md`](DESIGN.md) is the current
> specification and is the thing to read, review, and argue with.

## Purpose

Updating firmware over a slow or expensive link (RS-485, LoRa, NB-IoT, a
metered modem) is dominated by how many bytes you send. Most firmware releases
change a small fraction of the image, so sending the whole image every time is
mostly waste. Updating a device you cannot physically reach also means a failed
update has to be survivable — a bricked device at the end of a field cable is a
truck roll.

This project addresses both with one mechanism: the device tells the server
exactly what it already has, sector by sector, and the server sends only the
difference into a spare partition that the running firmware never depends on.

## The idea

1. **The device reports a manifest.** It hashes every flash sector it holds, in
   both partitions, and sends the list as `sector, hash`. On the reference part
   that is 104 sectors and about 15 ms of work, so the manifest is computed
   fresh every session and never cached.
2. **The server diffs and sends only what differs.** For each sector of the
   target image, if the device already has that content at that location,
   nothing is sent. If it has the content *somewhere else* in flash, the server
   names that location and the device copies it locally — still zero bytes on
   the wire. Otherwise the sector is sent as `sector, blob, hash`. A typical
   release lands around 10–15 % of a full image.
3. **Received sectors go straight into the inactive partition.** There is no
   separate staging area, because the inactive partition *is* the scratch
   space. That is what makes two 52 KB application slots fit in 128 KB of
   flash.
4. **The bootloader applies the plan and can unroll it.** Apply is journalled
   and resumable, and it never writes outside the inactive partition. So
   abandoning an update is one page erase, with nothing to undo.
5. **A new image gets a trial boot.** It must pass its own self-test and
   confirm. A crash, a hang, or an unreachable server costs an attempt; three
   failures and the device reverts to the partition it booted from before, then
   retries the update into the partition that just failed.

Integrity and authenticity are layered deliberately: per-sector hashes decide
*what to transfer*, one signed hash over the whole image decides *whether the
result is the real release*, the device signs its own manifest so the server
can trust what it is diffing against, and the session always runs over TLS.

## Read next

[`DESIGN.md`](DESIGN.md) covers it properly:

| Section | |
|---|---|
| 2 | Flash layout, meta banks, journal encoding |
| 4–5 | Manifest format and the three planner rules |
| 6 | Wire protocol and resume |
| 7–8 | Boot state machine, unroll, revert |
| 9 | Failure matrix |
| 10 | Image signing, manifest signing, TLS |
| 11 | Design decisions and their tradeoffs |
| 15 | Integrating with [MCUboot](https://github.com/mcu-tools/mcuboot) instead of the custom bootloader |

## Scope

Not planned: in-place single-partition updates, compression or binary diffing
inside a sector, multi-generation rollback, or over-the-air updates of the
bootloader itself. The reasoning for each is in `DESIGN.md`.
