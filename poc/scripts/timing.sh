#!/usr/bin/env bash
# Time spent per attempt. Host wall time is reported for what it is -- the host
# runs the same code far faster than the part -- next to a device-time model
# built from the operation counts: hashing at the cycles/byte in flash.h, flash
# erase and program at RM0008 typicals, and the ECDSA costs DESIGN.md budgets.
source "$(dirname "$0")/common.sh"
need_build
need_reference
fresh_run_dir
trap server_stop EXIT

cp "$KLIPPER_BIN" "$RUN/base.bin"
for runs in 1 4 12; do
    "$CB_FLASH" mutate "$RUN/base.bin" "$RUN/churn$runs.bin" "$runs" "$runs" >/dev/null
done

field() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p" <<<"$2"; }

row() {   # row <label> <log> <phase>
    local label="$1" line
    line="$(grep "^TIME phase=$3" "$2" | tail -1)"
    [ -n "$line" ] || return 0
    printf '%-22s %10s %12s %11s %9s %11s\n' "$label" \
        "$(field host_ms "$line")" "$(field model_ms "$line")" \
        "$(field flash_ms "$line")" "$(field erases "$line")" \
        "$(field prog_bytes "$line")"
}

hdr() {
    printf '\n%-22s %10s %12s %11s %9s %11s\n' \
        "attempt" "host ms" "F103 ms" "of it flash" "erases" "prog bytes"
    printf '%-22s %10s %12s %11s %9s %11s\n' \
        ---------------------- ---------- ------------ ----------- --------- -----------
}

try() {   # try <label> <target>
    local label="$1" target="$2"
    rm -f "$FLASH"; rm -rf "$CACHE"; mkdir -p "$CACHE"
    "$CB_FLASH" init "$FLASH" >/dev/null
    "$CB_FLASH" provision "$FLASH" 1 "$RUN/base.bin" 1 >/dev/null
    "$CB_FLASH" provision "$FLASH" 0 "$RUN/base.bin" 1 >/dev/null
    rm -f "$IMAGES"/*.bin; cp "$target" "$IMAGES/v2.bin"
    server_start
    "$CB_CLIENT" session --flash "$FLASH" --port "$PORT" >"$RUN/t.session"
    server_stop
    "$CB_CLIENT" boot --flash "$FLASH" >"$RUN/t.boot1"
    row "$label: session"  "$RUN/t.session" session
    row "$label: apply+verify" "$RUN/t.boot1" boot
    # Remaining trial boots, none of them confirmed.
    "$CB_CLIENT" boot --flash "$FLASH" >"$RUN/t.boot2"
    "$CB_CLIENT" boot --flash "$FLASH" >"$RUN/t.boot3"
    "$CB_CLIENT" boot --flash "$FLASH" >"$RUN/t.boot4"
    row "$label: trial 2"  "$RUN/t.boot2" boot
    row "$label: trial 3"  "$RUN/t.boot3" boot
    row "$label: revert"   "$RUN/t.boot4" boot
}

hdr
try "1 run"  "$RUN/churn1.bin"
try "4 runs" "$RUN/churn4.bin"
try "12 runs" "$RUN/churn12.bin"

echo
echo "steady-state boot with no update pending:"
"$CB_CLIENT" confirm --flash "$FLASH" >/dev/null 2>&1 || true
"$CB_CLIENT" boot --flash "$FLASH" >"$RUN/t.plain"
hdr
row "plain boot" "$RUN/t.plain" boot

echo
echo "phase detail, last 12-run session:"
grep '^time:' "$RUN/t.session"
echo
echo "model inputs: md5 ${F103_MD5:-10} cyc/B, sha256 25 cyc/B, 72 MHz,"
echo "              page erase 20 ms, half-word program 52.5 us (RM0008),"
echo "              ECDSA P-256 sign 250 ms / verify 300 ms (DESIGN.md 10.2)"
