#!/usr/bin/env bash
# Power loss mid-download, then resume: only the gap is re-sent.
source "$(dirname "$0")/common.sh"
need_build
need_reference
fresh_run_dir
trap server_stop EXIT

cp "$KLIPPER_BIN" "$IMAGES/v2.bin"              # target only, slot 1 is erased
"$CB_FLASH" init "$FLASH" >/dev/null
"$CB_FLASH" mutate "$KLIPPER_BIN" "$RUN/v1.bin" 8 99 >/dev/null
"$CB_FLASH" provision "$FLASH" 0 "$RUN/v1.bin" 1 >/dev/null

rule "first attempt, killed after 40 flash writes"
server_start --no-copy
set +e
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT" --fail-after 40
echo "device exited with $?"
set -e
server_stop
grep '^STATS' "$RUN/server.log" | tail -1

rule "resume: the device re-hashes its flash and the server re-diffs"
server_start --no-copy
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT"
server_stop
grep '^STATS' "$RUN/server.log" | tail -1

rule "apply and confirm"
"$CB_CLIENT" boot --flash "$FLASH"
"$CB_CLIENT" confirm --flash "$FLASH"
"$CB_CLIENT" status --flash "$FLASH"
