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

   The host PoC implements a single-partition variant instead: one 104 KB
   application, updated in place, which trades unroll and revert for the space.
   `DESIGN.md` section 18 has the comparison.
4. **The bootloader applies the plan and can unroll it.** Apply is journalled
   and resumable, and it never writes outside the inactive partition. So
   abandoning an update is one page erase, with nothing to undo.
5. **A new image gets a trial boot.** It must pass its own self-test and
   confirm. A crash, a hang, or an unreachable server costs an attempt; three
   failures and the device reverts to the partition it booted from before, then
   retries the update into the partition that just failed.

Break-even against simply sending the whole image sits at 50 of 52 sectors, so
delta loses only when essentially everything changed, and then by under 4 %. A
one-sector change costs 2.7 % of a full transfer.

## Transports

The protocol is defined as messages; how they reach the device is a swappable
link adapter. TCP/TLS and UART/RS-485 are byte streams. **Modbus** maps onto a
holding-register mailbox with the master driving every exchange. **CAN** uses
ISO-TP, with update traffic given a low-priority identifier so control frames
always win arbitration — and if the product already speaks UDS, the plan maps
straight onto `RequestDownload`/`TransferData`. Devices on a field bus are
reached through a gateway; the signatures are end to end, so the gateway never
has to be trusted.

Integrity and authenticity are layered deliberately: per-sector hashes decide
*what to transfer*, one signed hash over the whole image decides *whether the
result is the real release*, the device signs its own manifest so the server
can trust what it is diffing against, and the session always runs over TLS.

## Proof of concept

[`poc/`](poc/) is a working host implementation of one update request: a
server, a device client, and a 128 KB emulated STM32F103CB flash with page
erase and programming that refuses to set a bit without an erase. It transfers
a real 45 772 B firmware binary sector by sector and applies it through the
bootloader path, reporting which sectors differed, what went over the wire
against a full transfer, and what the device spends in flash. Writing it
corrected three errors in the design.

```sh
cd poc
make
scripts/run_demo.sh
```

[`poc/README.md`](poc/README.md) has the architecture diagrams, the message
exchange, and what the PoC deliberately leaves out.

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
| 16 | Transports: byte stream, Modbus, CAN with ISO-TP, UDS |
| 17 | Overhead against a full image transfer, and manifest elision |

## For Review/ TODO
1. In-place single-partition updates;
2. Compression or binary diffing;
3. Multi-generation rollback or over-the-air updates of the bootloader itself;
4. Reasoning for each is in `DESIGN.md`.



## Running the PoC by hand

There is no setup step: the device is created on first use with the reference
binary in slot 0, so its first manifest describes real firmware.

Terminal 1, left running:

```sh
cd poc
scripts/lab.sh server
```

Terminal 2:

```sh
scripts/lab.sh release 4        # publish a release with 4 patched runs
scripts/lab.sh update           # one request: manifest, delta, apply, confirm
scripts/lab.sh status
```

Or one terminal, backgrounding the server in your own shell:

```sh
scripts/lab.sh server &
scripts/lab.sh release 4 && scripts/lab.sh update
kill %1
```

The server stays up across requests, re-reads the release file each time, and
prints one stats row per request, so `release` then `update` again adds another
row. It exits with the shell that started it, so background it from your own
shell rather than from a script that then exits. `scripts/lab.sh reset` forgets
the device so it ships fresh again.

