# chunky_mcu_updater

Remote firmware updater for small microcontrollers that transfers **only the flash chunks that actually
changed**, and that can always get the device back to the firmware it was running before the update started.

Fork of Roman Skakun's [chunky_mcu_updater](https://github.com/rmskkn/chunky_mcu_updater). His design is in
[DESIGN.md](DESIGN.md), unchanged. This fork builds it in Rust on top of
[embassy-boot](https://github.com/embassy-rs/embassy/tree/main/embassy-boot) and on hardware bank swap, generic
across STM32F1, G0, H7 and RP2350.

> **Status: first prototype, host only.** The delta layer, the embassy-boot swap backend, the bank swap backend
> and the planner run in a flash simulator and pass power-cut sweeps on every family model. No hardware and no
> transport yet.
> [FEATURES.md](FEATURES.md) has the state of every piece.

## Purpose

Updating firmware over a slow or expensive link (RS-485, LoRa, NB-IoT, CAN, a metered modem) is dominated by how
many bytes you send. Most releases change a small fraction of the image. And a device you cannot reach must
survive a failed update.

## How it works

1. **The device reports a manifest**: a truncated SHA-256 per chunk of its running image (ACTIVE) and of its
   staging area (DFU), hashed fresh every session. The manifest is the only progress record; there is no journal.
2. **The server plans.** Per erase block of the destination: leave it, fill in its blank chunks, or erase it and
   program it. A chunk the running image already holds anywhere is **copied** on the device (zero bytes on the
   wire); only chunks the device does not have are **written** from the wire.
3. **The device applies the plan into DFU**, trusting none of it: order, ranges and hashes are checked, only blank
   chunks are programmed, and the whole image must verify against its signed descriptor before a swap is asked for.
4. **The bootloader swaps** (embassy-boot, power-fail safe), after verifying the staged image itself. The new
   image gets **one trial boot**: it confirms, or the next reset reverts it. On dual-bank parts nothing is
   swapped: the bootloader verifies the other bank and flips the SWAP_BANK option bit, and a revert flips it back.
5. **A power cut anywhere** just means the device boots what it has, hashes again and gets a plan for what is
   still missing.

## What differs from DESIGN.md

| DESIGN.md | Here | Why |
|---|---|---|
| Custom A/B bootloader, two builds (one per slot) | embassy-boot swap, one build at one address | Every unchanged page of the new image equals a page of the running one, so COPY hits all unchanged code |
| Meta banks, apply journal | None: DFU's own contents are the progress | Nothing to disagree with the flash |
| Sector = erase page = delta unit, F103 numbers | Separate chunk, erase and write sizes, const-checked per chip | G0, H7 (128 KB sectors), RP2350; an unsupported layout fails the build |
| MD5 per sector | SHA-256 truncated to 16 bytes | No weak primitive on the device |
| ECDSA P-256, version compared with the running one | Ed25519, security counter against a stored floor that only rises | Pure Rust; a revert does not lower the bar |
| Hash of the slot checked on boot | Signature checked on every boot | The stored hash sits in the slot it covers |
| COPY from any slot | COPY from the running image only | It cannot be overwritten by an earlier step |
| Device-signed manifest, TLS | Not yet (SEC-4, NET-1) | Every step is verified on the device anyway |

The review behind these choices found that the original's journal encoding, commit marker and rollback rule
would not hold on real F103 flash.

## Security in one paragraph

Images are sealed with an Ed25519 signature over a descriptor that names product, version, security counter,
flash layout and the SHA-256 of the whole partition. The bootloader verifies before a swap and on every boot, so
unsigned code does not survive a reset; a version floor that only the bootloader raises stops rollbacks; plans,
manifests, the server and the transport are untrusted. What a compromised application can still do depends on
the chip (on F103: everything, there is no RDP level 2). [SECURITY.md](SECURITY.md) has the full picture.

**Unsigned builds.** Where the verifier does not fit, `--features unsigned` leaves it out, by name and never by
default. Integrity (SHA-256) and the version floor stay. **An unsigned device is protected against bad luck and
broken transfers, not against anyone who can talk to it**: whoever reaches the update channel can install any
firmware for that product. The device reports it, the planner warns about every such device, and a device that
trusts a key never accepts an unsigned image.

## Crates

| Crate | |
|---|---|
| `chunky` | `no_std` core: geometry, manifest, plan format, apply engine, descriptor, version floor, ECC-tolerant reads |
| `chunky_swap` | `no_std` embassy-boot backend: the bootloader's decision and the application's handle |
| `chunky_bank` | `no_std` hardware bank swap backend (dual-bank STM32): the same two things over a SWAP_BANK option bit |
| `chunky_server` | Image builder and sealer, content-addressed blob store, planner |
| `chunky_cli` | The `chunky` command: keygen, image build, manifest, plan, store (a thin layer over `chunky_server`) |
| `chunky_sim` | Flash simulator (F1, G0, H7, RP2350 write rules, torn writes, ECC faults) and a simulated device |

```sh
just test    # unit tests, planner properties, security cases, power-cut sweeps
just lint    # clippy, fmt, firmware-target builds (signed and unsigned)
CHUNKY_SWEEP_SEEDS=500 just test   # tear every operation 500 ways
```

## The `chunky` command

```sh
just install                                   # builds and installs `chunky`
chunky keygen release.key                      # mode 600; prints the public key for the bootloader
chunky image build app.bin -o app-1.img --chip stm32g0b1re --key release.key --product 7 --version 1
chunky image build app2.bin -o app-2.img --chip stm32g0b1re --key release.key --product 7 --version 2
chunky plan --chip stm32g0b1re --from app-1.img --to app-2.img -o update.plan
                                               # skip / copy / write / erase counts and bytes on the wire
chunky plan --chip stm32g0b1re --manifest device.manifest --to app-2.img
chunky manifest device.manifest --chip stm32g0b1re
chunky store add app-2.img --store ./blobs --chip stm32g0b1re ; chunky store ls --store ./blobs
chunky chips                                   # presets
chunky completions fish > ~/.config/fish/completions/chunky.fish   # also bash, zsh
```

`--unsigned` builds an image for devices built with the `unsigned` opt-out and warns about it. The key file is
created with mode 600, never overwritten, and refused when group or others can read it; the CLI never prints it.
Colour is off when the output is not a terminal or `NO_COLOR` is set.

## Supported chips

A firmware build picks one `chip-*` feature or brings its own const-checked `FlashGeometry` (`chip-custom`);
anything else is a compile error.

| Chip | Chunk / erase / write | Backend | State |
|---|---|---|---|
| STM32F103CB | 1 KB / 1 KB / 16 bit | embassy-boot swap | simulated |
| STM32G0B1RE | 2 KB / 2 KB / 64 bit + ECC | embassy-boot swap | simulated; first hardware (HW-1) |
| STM32H743ZI | 2 KB / 128 KB / 256 bit + ECC | hardware bank swap | simulated; hardware is HW-2 |
| RP2350 / RP2354 | 4 KB / 4 KB / byte | boot ROM A/B | geometry and flash model; backend is BOOT-3 |

## Not planned

In-place single-partition updates, binary diffing inside a chunk, multi-generation rollback, and updates of the
bootloader itself.
