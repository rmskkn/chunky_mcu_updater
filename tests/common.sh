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

# Geometry comes from cb-flash geometry (src/cbflash.c's cmd_geometry()): ERASE_SIZE, SECTOR_SIZE, SECTORS, SLOT_SIZE, PAYLOAD_MAX, geometry_id. No RAW_WIRE/COPY_MARGIN (format2 has no per-fragment/PLAN_TABLE framing to margin for) and no SECTOR_HASH_LEN (format2_chunk_hash() fixes it at 8 B); referencing it here crashed under `set -eu` once cmd_geometry() stopped printing it.
need_build() {
    [ -x "$CB_FLASH" ] || make -C "$POC" >/dev/null
    eval "$("$CB_FLASH" geometry)"
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

# The case's own stderr: a failure inside update() still reaches it although
# callers capture update's output with 2>&1.
exec 3>&2

# server_start [ARGS...]: ARGS go to cb-server (test hooks).
server_start() {
    "$CB_SERVER" --port "$PORT" --image "$IMAGE" --cache "$CACHE" "$@" \
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

# server_idle: wait until the server has ended every session it started.
# The device side is done at COMMIT (or ABORT) and does not wait for the
# server, which writes the rest of that session's log, its report last,
# after that. A case that reads server.log after a session calls this first;
# update() does. A client that received any frame was accepted, and the
# server logs "session start" before it reads HELLO. IDLE_MARK, when a case
# sets it, names a file created once this has seen a session still open:
# proof that the caller waited (case_server_log_settled.sh).
server_idle() {
    local log="$RUN/server.log"
    for _ in $(seq 200); do
        [ "$(grep -c '^server: session start' "$log")" -eq "$(grep -c '^server: session end' "$log")" ] && return 0
        [ -z "${IDLE_MARK:-}" ] || : >"$IDLE_MARK"
        if [ -n "${SERVER_PID:-}" ] && ! kill -0 "$SERVER_PID" 2>/dev/null; then
            fail "server" "the server exited inside a session; see $log" 2>&3
        fi
        sleep 0.05
    done
    fail "server" "a session did not end within 10 s; see $log" 2>&3
}

update() {
    local rc=0
    "$CB_CLIENT" --flash "$FLASH" --initial "$BASE" --port "$PORT" "$@" || rc=$?
    server_idle
    return $rc
}

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

# format2 has no mid-slot descriptor hole (unlike the old lib/image.h layout): its 128 B descriptor is appended after the raw binary, in the slot's *last* 128 bytes, so a sector's bin offset is simply `sector * SECTOR_SIZE`, for every sector including 0; these helpers never need to avoid the descriptor region since BASE/IMAGE stays well under PAYLOAD_MAX.

# patch_sector BIN SECTOR BYTE: overwrite sector index SECTOR's range with one repeated byte.
patch_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
sector = $2
start = sector * SECT
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
# WRITE step.
scramble_sector() {
    python3 -I -c "
import random
SECT = $SECTOR_SIZE
sector = $2
random.seed($3)
start = sector * SECT
end = start + SECT
data = bytearray(open('$1', 'rb').read())
data[start:end] = bytes(random.randrange(1, 255) for _ in range(SECT))
open('$1', 'wb').write(data)
"
}

# motif_sector BIN SECTOR: fill the sector with a repeating 16-byte motif:
# compressible, but not one repeated byte.
motif_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
start = $2 * SECT
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
at = $2 * SECT + SECT // 2
data = open('$1', 'rb').read()
open('$1', 'wb').write((data[:at] + bytes([0x77]) * $3 + data[at:])[:len(data)])
"
}

# tweak_sector BIN SECTOR [XOR]: change 16 bytes in the middle of SECTOR.
tweak_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
at = $2 * SECT + SECT // 2
data = bytearray(open('$1', 'rb').read())
for i in range(16): data[at + i] ^= ${3:-0x55}
open('$1', 'wb').write(data)
"
}

# sector_hash_hex FLASH INDEX: the manifest's chunk hash at flat INDEX (0..chunks-1 active slot, chunks..2*chunks-1 dfu slot). Tag-aware: BLANK/UNREADABLE carry no hash, so this prints empty rather than a bogus value.
sector_hash_hex() {
    "$CB_FLASH" manifest "$1" | python3 -I -c "
import sys
want = '$2'
t = sys.stdin.read().split()
for i in range(0, len(t) - 1, 2):
    if t[i] == want:
        v = t[i + 1]
        print('' if v in ('BLANK', 'UNREADABLE') else v)
        break
"
}

# copy_sector BIN DST SRC: overwrite sector DST's range with sector SRC's
# current bytes.
copy_sector() {
    python3 -I -c "
SECT = $SECTOR_SIZE
dst, src = $2, $3
def rng(s): start = s * SECT; return start, start + SECT
data = bytearray(open('$1', 'rb').read())
ds, de = rng(dst); ss, se = rng(src)
data[ds:de] = bytes(data[ss:se])
open('$1', 'wb').write(data)
"
}

# plan_counts: sets ERASE_N/COPY_N/WRITE_N/OF_N/TX_B/RX_B from the last "vN erase copy write of txB rxB" row server.c's report() prints (one row per request). format2 has no compression counters to read alongside these.
plan_counts() {
    local row
    row="$(grep -E '^v[0-9]+ +[0-9]' "$RUN/server.log" | tail -1)"
    [ -n "$row" ] || fail "plan_counts" "no plan report row (label vN ...) in $RUN/server.log"
    read -r _ ERASE_N COPY_N WRITE_N OF_N TX_B RX_B <<<"$row"
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
