#!/usr/bin/env bash
# format2.c (lib/) against the golden vectors and the negative cases of
# tests/format2_test.c: the read-only decoder for chunky's "format 2" wire
# contract (SPEC.md; rust/chunky_format/c holds the generated views it
# is built on, vendored unmodified from github.com/rmskkn/chunky_mcu_updater
# PR #5). A unit test, like tests/compress_test.c (test-compress), not an
# end-to-end server/client case, so it does not source common.sh; builds
# and runs through `make test-format2`, the single source of truth for the
# build command (Makefile).
set -eu
cd "$(dirname "$0")/.."

ok()   { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s: %s\n' "$1" "$2" >&2; exit 1; }

out="$(make test-format2 2>&1)" || fail "format2" "$out"
case "$out" in
    *"format2_test: ok"*) ok "format2" ;;
    *) fail "format2" "$out" ;;
esac
