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

# Geometry comes from the binary under test (cb-flash geometry); RAW_WIRE is
# what one uncompressed sector costs to receive.
need_build() {
    [ -x "$CB_FLASH" ] || make -C "$POC" >/dev/null
    eval "$("$CB_FLASH" geometry)"
    RAW_WIRE=$((SECTOR_SIZE / 256 * 272))   # per 256 B fragment: frame 10 + header 6
    # A/B margin: on a device's first update the destination slot starts
    # blank, so almost every sector the plan doesn't write is a free
    # OP_COPY instead of an "already right" skip -- zero bytes each, but
    # still a framed STEP_COPY + STEP_ACK, and an entry in the plan's table
    # (SECTOR_HASH_LEN per sector, sent for every sector of the image).
    # Tests that bound total wire size after a first-ever update add this
    # per-sector margin on top of whatever the write-sized sectors actually
    # cost.
    COPY_MARGIN=$((SECTORS * (40 + 2 * SECTOR_HASH_LEN) + 700))
}

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

# wire_stats TEXT: parses the client's "device: wire tx=.. rx=.. bytes
# (framing overhead tx=.. rx=.. B over ../.. frames)" line out of TEXT (as
# captured by `out="$(update 2>&1)"`). Sets WIRE_TX/WIRE_RX/WIRE_OVH_TX/
# WIRE_OVH_RX for the caller, prints a verbose per-request stats line, and
# appends to the run's shared log so run_all.sh can print a grand total.
# No-op (and leaves the WIRE_* vars unset) if TEXT has no such line, e.g. a
# captured block that never reached the server.
wire_stats() {
    local text="$1" raw pct_raw pct_ovh
    WIRE_TX="$(sed -n 's/.*wire tx=\([0-9]*\).*/\1/p' <<<"$text" | tail -1)"
    WIRE_RX="$(sed -n 's/.*rx=\([0-9]*\) bytes.*/\1/p' <<<"$text" | tail -1)"
    WIRE_OVH_TX="$(sed -n 's/.*overhead tx=\([0-9]*\).*/\1/p' <<<"$text" | tail -1)"
    WIRE_OVH_RX="$(sed -n 's/.*overhead tx=[0-9]* rx=\([0-9]*\) B.*/\1/p' <<<"$text" | tail -1)"
    [ -n "${WIRE_RX:-}" ] || return 0
    raw=$((SECTORS * SECTOR_SIZE))
    pct_raw="$(awk -v rx="$WIRE_RX" -v raw="$raw" 'BEGIN{printf "%.1f", 100*rx/raw}')"
    pct_ovh="$(awk -v o="${WIRE_OVH_RX:-0}" -v rx="$WIRE_RX" 'BEGIN{printf "%.1f", (rx>0) ? 100*o/rx : 0}')"
    printf '      wire: tx=%s B (%s B framing)  rx=%s B (%s B framing, %s%% of rx)  -- %s%% of a %d B raw image\n' \
        "$WIRE_TX" "${WIRE_OVH_TX:-0}" "$WIRE_RX" "${WIRE_OVH_RX:-0}" "$pct_ovh" "$pct_raw" "$raw"
    mkdir -p "$TESTS/run"
    echo "$WIRE_TX $WIRE_RX ${WIRE_OVH_TX:-0} ${WIRE_OVH_RX:-0} $raw" >>"$TESTS/run/.wire.log"
}

# gen_random_bin OUT SIZE SEED: a deterministic, non-uniform firmware payload.
# Self-contained (no external reference binary), so a test's content is
# reproducible from the test alone, and plain enough that no sector-sized window
# of it is ever accidentally all-0xFF or all-0x00.
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
# of SECTOR_SIZE. Mirrors the slot layout in lib/image.h: a target byte at
# offset T (T >= CODE_OFF=640) comes from bin offset T-DESC_LEN.
patch_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
sector = $2
start = sector * SECT - $DESC_LEN
end = start + SECT
data = bytearray(open('$1', 'rb').read())
data[start:end] = bytes([$3]) * SECT
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
SECT = $SECTOR_SIZE
sector = $2
random.seed($3)
start = sector * SECT - $DESC_LEN
end = start + SECT
data = bytearray(open('$1', 'rb').read())
data[start:end] = bytes(random.randrange(1, 255) for _ in range(SECT))
open('$1', 'wb').write(data)
"
}

# motif_sector BIN SECTOR: fill the sector with a repeating 16-byte motif:
# compressible, but not one repeated byte. SECTOR >= 1, as above.
motif_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
start = $2 * SECT - $DESC_LEN
motif = bytes((i * 7 + 3) % 251 + 1 for i in range(16))
data = bytearray(open('$1', 'rb').read())
data[start:start + SECT] = motif * (SECT // 16)
open('$1', 'wb').write(data)
"
}

# insert_bytes BIN SECTOR N: insert N bytes inside SECTOR and truncate to the
# old length, so everything after it shifts by N: a release that grew some code.
insert_bytes() {
    python3 -I -c "
SECT = $SECTOR_SIZE
at = $2 * SECT - $DESC_LEN + SECT // 2
data = open('$1', 'rb').read()
open('$1', 'wb').write((data[:at] + bytes([0x77]) * $3 + data[at:])[:len(data)])
"
}

# tweak_sector BIN SECTOR [XOR]: change 16 bytes in the middle of SECTOR.
tweak_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
at = $2 * SECT - $DESC_LEN + SECT // 2
data = bytearray(open('$1', 'rb').read())
for i in range(16): data[at + i] ^= ${3:-0x55}
open('$1', 'wb').write(data)
"
}

# sector_hash_hex FLASH SECTOR: the manifest hash of one sector of a device.
sector_hash_hex() {
    "$CB_FLASH" manifest "$1" | python3 -I -c "
import sys
t = sys.stdin.read().split()
i = t.index('$2')
print(t[i + 1])
"
}

# copy_sector BIN DST SRC: overwrite sector DST's range with sector SRC's
# current bytes, both >= 1 (see patch_sector).
copy_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
dst, src = $2, $3
def rng(s): start = s * SECT - $DESC_LEN; return start, start + SECT
data = bytearray(open('$1', 'rb').read())
ds, de = rng(dst); ss, se = rng(src)
data[ds:de] = bytes(data[ss:se])
open('$1', 'wb').write(data)
"
}

# compress_count raw|lz4|lz4_dict: sectors the server's last session sent that way.
compress_count() {
    grep 'server: compress' "$RUN/server.log" | tail -1 | sed -n "s/.* $1=\([0-9]*\).*/\1/p"
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
