#!/usr/bin/env bash
# End to end: provision v1, update to v2 over the wire, boot, confirm.
source "$(dirname "$0")/common.sh"
need_build
need_reference
fresh_run_dir
trap server_stop EXIT

cp "$KLIPPER_BIN" "$IMAGES/v1.bin"
"$CB_FLASH" mutate "$IMAGES/v1.bin" "$IMAGES/v2.bin" 8 1234 >/dev/null

rule "flash map"
"$CB_FLASH" init "$FLASH"
"$CB_FLASH" map "$FLASH"

rule "provision slot 0 with v1, slot 1 left erased"
"$CB_FLASH" provision "$FLASH" 0 "$IMAGES/v1.bin" 1
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" status --flash "$FLASH"

rule "update session: v1 -> v2"
mv "$IMAGES/v1.bin" "$RUN/v1.bin.keep"          # offer v2 as the newest
server_start
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT"
server_stop
grep -E '^(server: (steps|wire|plan|manifest))' "$RUN/server.log" || true

rule "bootloader applies the plan"
"$CB_CLIENT" boot --flash "$FLASH"

rule "self-test passes, confirm"
"$CB_CLIENT" confirm --flash "$FLASH"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" status --flash "$FLASH"

rule "second session: nothing to do"
server_start
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT"
server_stop
