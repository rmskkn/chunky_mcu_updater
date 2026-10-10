#!/usr/bin/env bash
# Builds the release set the server offers, from the reference binary.
#   v1  the reference binary as shipped
#   v2  a localized change: 8 patched runs of 16 bytes
#   v3  the same change plus 16 bytes inserted near the start, which shifts
#       everything after it and destroys sector alignment (DESIGN.md 17)
source "$(dirname "$0")/common.sh"
need_build
need_reference

mkdir -p "$IMAGES"
cp "$KLIPPER_BIN" "$IMAGES/v1.bin"
"$CB_FLASH" mutate "$IMAGES/v1.bin" "$IMAGES/v2.bin" 8 1234
"$CB_FLASH" shift  "$IMAGES/v2.bin" "$IMAGES/v3.bin" 16

printf '\nreference %s, %s B\n' "$KLIPPER_BIN" "$(stat -c %s "$KLIPPER_BIN")"
ls -l "$IMAGES"
