#!/usr/bin/env bash
# Shared setup for the test cases in this directory. Sourced, not executed.
#
# Each case gets its own scratch dir under tests/run/<name> and its own port,
# so cases never share a flash file, a manifest cache or a listening socket -
# run them back to back or in parallel, same result.
set -eu

TESTS="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
POC="$(cd "$TESTS/.." && pwd)"
BUILD="$POC/build"

CB_FLASH="$BUILD/cb-flash"
CB_SERVER="$BUILD/cb-server"
CB_CLIENT="$BUILD/cb-client"

need_build() { [ -x "$CB_FLASH" ] || make -C "$POC" >/dev/null; }

# test_init NAME: sets RUN, CACHE, FLASH, IMAGE, BASE, PORT and clears RUN.
test_init() {
    local name="$1"
    RUN="$TESTS/run/$name"
    CACHE="$RUN/cache"
    FLASH="$RUN/flash.bin"
    IMAGE="$RUN/release.bin"
    BASE="$RUN/base.bin"
    PORT=$((20000 + (RANDOM % 10000)))
    rm -rf "$RUN"
    mkdir -p "$CACHE"
}

server_start() {
    "$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" \
        >"$RUN/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 50); do
        grep -q listening "$RUN/server.log" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "server did not start:" >&2; cat "$RUN/server.log" >&2; exit 1
}

server_stop() {
    [ -n "${SERVER_PID:-}" ] || return 0
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=
}

update() { "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" "$@"; }

# gen_random_bin OUT SIZE SEED: a deterministic, non-uniform firmware payload.
# Self-contained (no external reference binary), so a test's content is
# reproducible from the test alone, and plain enough that no 1 KB window of
# it is ever accidentally all-0xFF or all-0x00.
gen_random_bin() {
    python3 -I -c "
import random, sys
random.seed($3)
open('$1', 'wb').write(bytes(random.randrange(1, 255) for _ in range($2)))
"
}

# patch_sector BIN SECTOR BYTE: overwrite sector index SECTOR's slot-image
# range with one repeated byte. SECTOR must be >= 1 - sector 0 straddles the
# vector table and the descriptor, so its bin offset is not a clean multiple
# of PAGE_SIZE. Mirrors the slot layout in lib/image.h: a target byte at
# offset T (T >= CODE_OFF=640) comes from bin offset T-128.
patch_sector() {
    python3 -I -c "
PAGE = 1024
sector = $2
start = sector * PAGE - 128
end = start + PAGE
data = bytearray(open('$1', 'rb').read())
data[start:end] = bytes([$3]) * PAGE
open('$1', 'wb').write(data)
"
}

# scramble_sector BIN SECTOR SEED: overwrite sector index SECTOR's range
# with fresh non-uniform pseudo-random bytes - an ordinary change that is
# neither fillable (not one repeated byte) nor a dedup (new random content
# does not already exist elsewhere), so the planner must send it as a plain
# STEP_WRITE. SECTOR must be >= 1 (see patch_sector).
scramble_sector() {
    python3 -I -c "
import random
PAGE = 1024
sector = $2
random.seed($3)
start = sector * PAGE - 128
end = start + PAGE
data = bytearray(open('$1', 'rb').read())
data[start:end] = bytes(random.randrange(1, 255) for _ in range(PAGE))
open('$1', 'wb').write(data)
"
}

# copy_sector BIN DST SRC: overwrite sector DST's range with sector SRC's
# current bytes, both >= 1 (see patch_sector).
copy_sector() {
    python3 -I -c "
PAGE = 1024
dst, src = $2, $3
def rng(s): start = s * PAGE - 128; return start, start + PAGE
data = bytearray(open('$1', 'rb').read())
ds, de = rng(dst); ss, se = rng(src)
data[ds:de] = bytes(data[ss:se])
open('$1', 'wb').write(data)
"
}

ok()   { printf 'PASS  %s\n' "$1"; }
fail() { printf 'FAIL  %s: %s\n' "$1" "$2" >&2; exit 1; }

assert_contains() {       # text needle label
    grep -qF -- "$2" <<<"$1" || fail "${3:-$2}" "expected to find: $2"
}
assert_not_contains() {   # text needle label
    if grep -qF -- "$2" <<<"$1"; then fail "${3:-$2}" "did not expect: $2"; fi
    return 0
}
