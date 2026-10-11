#!/usr/bin/env bash
# One update request, end to end. The device reports what is in its flash, the
# server sends only the sectors that differ, the bootloader applies them.
#
# The server stays up, so each release here is another request and another row
# in its stats table.
source "$(dirname "$0")/common.sh"
need_build
need_reference

rm -rf "$RUN"; mkdir -p "$CACHE"
cp "$KLIPPER_BIN" "$BASE"
cp "$BASE" "$IMAGE"
trap server_stop EXIT

server_start

rule "a release with 4 patched runs"
"$CB_FLASH" mutate "$BASE" "$IMAGE" 4 1234 >/dev/null
update 2>&1 | grep -Ev '^time:'

rule "a release with 12 patched runs"
"$CB_FLASH" mutate "$BASE" "$IMAGE" 12 12 >/dev/null
update 2>&1 | grep -E 'plan v|booting'

rule "the same release again: the manifest already matches it"
update 2>&1 | grep -E 'no update|booting'

rule "flash layout"
"$CB_FLASH" map "$FLASH"

server_stop
rule "what the server saw"
cat "$RUN/server.log"
