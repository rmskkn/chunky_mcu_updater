#!/usr/bin/env bash
# Plan authentication and meta integrity: a forged signature, table, step
# order, descriptor or meta bank is refused, and an honest plan still boots.
# The cases are in tests/auth_test.c, which drives the device core directly
# (a real server never sends these messages).
source "$(dirname "$0")/common.sh"
need_build
test_init auth

gen_random_bin "$BASE" $((PAYLOAD_MAX * 5 / 10)) 801
[ -x "$BUILD/auth_test" ] || make -C "$POC" >/dev/null
"$BUILD/auth_test" "$RUN" "$BASE" || fail "auth" "see above"
