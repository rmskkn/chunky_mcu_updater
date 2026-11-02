#!/usr/bin/env bash
# format2.c (lib/) against the golden vectors and the negative cases of
# tests/format2_test.c: the read-only decoder for chunky's "format 2" wire
# contract (SPEC.md; rust/chunky_format/c holds the generated accessors it
# is built on, vendored unmodified from github.com/romixlab/chunky_mcu_updater
# PR #5). A unit test, like tests/compress_test.c, not an end-to-end
# server/client case, so it does not source common.sh. Builds itself here
# with cc/CFLAGS rather than through the Makefile: nothing in Makefile was
# changed to add this test, same reason PR #5's own chunky_format reader
# (`make vectors`) builds without touching the rest of the project.
set -eu
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
CFLAGS="-std=c99 -Wall -Wextra -Werror -pedantic -Wconversion -Wsign-conversion"
BUILD=build
BIN="$BUILD/format2_test"

mkdir -p "$BUILD"
# shellcheck disable=SC2086
"$CC" $CFLAGS -iquote rust/chunky_format/c -iquote lib \
    -o "$BIN" tests/format2_test.c lib/format2.c lib/sha256.c

ok()   { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s: %s\n' "$1" "$2" >&2; exit 1; }

out="$("$BIN" 2>&1)" || fail "format2" "$out"
case "$out" in
    *"format2_test: ok"*) ok "format2 ($out)" ;;
    *) fail "format2" "$out" ;;
esac
