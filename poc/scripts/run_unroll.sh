#!/usr/bin/env bash
# Two failure paths: a tampered image that passes every per-sector check, and
# a plan that is never committed. Both must leave the device on v1.
source "$(dirname "$0")/common.sh"
need_build
need_reference
fresh_run_dir
trap server_stop EXIT

"$CB_FLASH" init "$FLASH" >/dev/null
"$CB_FLASH" mutate "$KLIPPER_BIN" "$RUN/v1.bin" 8 7 >/dev/null
cp "$KLIPPER_BIN" "$IMAGES/v2.bin"
"$CB_FLASH" provision "$FLASH" 0 "$RUN/v1.bin" 1 >/dev/null

rule "server tampers with sector 9 and advertises a matching md5"
server_start --tamper 9
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT"
server_stop

rule "every sector verified, so only the image-level gate can catch it"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" status --flash "$FLASH"

rule "trial boot that is never confirmed reverts after 3 attempts"
rm -rf "$CACHE"; mkdir -p "$CACHE"
server_start
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT"
server_stop
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" status --flash "$FLASH"
