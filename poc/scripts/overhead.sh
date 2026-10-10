#!/usr/bin/env bash
# Measures real wire bytes against a full image transfer, for several churn
# levels. Reproduces the table in DESIGN.md section 17 from actual traffic.
source "$(dirname "$0")/common.sh"
need_build
need_reference
fresh_run_dir
trap server_stop EXIT

measure() {   # measure <label> <prep: steady|fresh> <target-bin> <server-flags...>
    local label="$1" prep="$2" target="$3"; shift 3
    rm -f "$FLASH"; rm -rf "$CACHE"; mkdir -p "$CACHE"
    "$CB_FLASH" init "$FLASH" >/dev/null
    # steady state: the destination slot already holds the previous release,
    # which is the case DESIGN.md section 17 models. fresh: it is erased.
    [ "$prep" = steady ] && "$CB_FLASH" provision "$FLASH" 1 "$RUN/base.bin" 1 >/dev/null
    "$CB_FLASH" provision "$FLASH" 0 "$RUN/base.bin" 1 >/dev/null
    rm -f "$IMAGES"/*.bin
    cp "$target" "$IMAGES/v2.bin"
    server_start "$@"
    "$CB_CLIENT" session --flash "$FLASH" --port "$PORT" >/dev/null
    server_stop
    emit_row "$label"
}

# Pulls both machine-readable lines the server prints and formats one row.
emit_row() {
    local label="$1" st sv
    st="$(grep '^STATS' "$RUN/server.log" | tail -1)"
    sv="$(grep '^SAVE'  "$RUN/server.log" | tail -1)"
    [ -n "$st" ] || return 0
    local skip copy write wire saved pct fmin fmax
    skip=$(field skip "$st"); copy=$(field copy "$st"); write=$(field write "$st")
    wire=$(field wire "$st")
    saved=$(field saved_bytes "$sv"); pct=$(field saved_pct "$sv")
    fmin=$(field flash_min_ms "$sv"); fmax=$(field flash_max_ms "$sv")
    printf '%-26s %4s %4s %5s %8s %7s %9s %7s %13s\n' \
        "$label" "$skip" "$copy" "$write" "$wire" \
        "$(awk -v w="$wire" -v f="$FULL" 'BEGIN{printf "%.1f%%", 100*w/f}')" \
        "$saved" "$pct%" "$fmin-$fmax"
}

field() { sed -n "s/.*[ =]$1=\([0-9.]*\).*/\1/p" <<<"$2"; }

# Same scenario but with a warm server-side manifest cache, so the body is
# elided (DESIGN.md 17.1). Priming runs a session whose target equals the
# running version: the server caches the manifest, then answers NO_UPDATE
# without the device writing anything, so the cached digest stays valid.
measure_warm() {
    local label="$1" target="$2"; shift 2
    rm -f "$FLASH"; rm -rf "$CACHE"; mkdir -p "$CACHE"
    "$CB_FLASH" init "$FLASH" >/dev/null
    "$CB_FLASH" provision "$FLASH" 1 "$RUN/base.bin" 1 >/dev/null
    "$CB_FLASH" provision "$FLASH" 0 "$RUN/base.bin" 1 >/dev/null
    rm -f "$IMAGES"/*.bin; cp "$RUN/base.bin" "$IMAGES/v1.bin"
    server_start
    "$CB_CLIENT" session --flash "$FLASH" --port "$PORT" >/dev/null
    server_stop
    rm -f "$IMAGES"/*.bin; cp "$target" "$IMAGES/v2.bin"
    server_start
    "$CB_CLIENT" session --flash "$FLASH" --port "$PORT" >/dev/null
    server_stop
    emit_row "$label"
}

cp "$KLIPPER_BIN" "$RUN/base.bin"
for runs in 1 4 12 40; do
    "$CB_FLASH" mutate "$RUN/base.bin" "$RUN/churn$runs.bin" "$runs" "$runs" >/dev/null
done
"$CB_FLASH" shift "$RUN/base.bin" "$RUN/shifted.bin" 16 >/dev/null

# Baseline: the same transport sending every sector.
FULL=1
rm -f "$FLASH"; rm -rf "$CACHE"; mkdir -p "$CACHE"
"$CB_FLASH" init "$FLASH" >/dev/null
"$CB_FLASH" provision "$FLASH" 1 "$RUN/base.bin" 1 >/dev/null
"$CB_FLASH" provision "$FLASH" 0 "$RUN/base.bin" 1 >/dev/null
rm -f "$IMAGES"/*.bin; cp "$RUN/churn4.bin" "$IMAGES/v2.bin"
server_start --full
"$CB_CLIENT" session --flash "$FLASH" --port "$PORT" >/dev/null
server_stop
FULL="$(grep '^STATS' "$RUN/server.log" | tail -1 | sed -n 's/.*wire=\([0-9]*\).*/\1/p')"

printf '\nreference %s, slot payload %s B\n' "$(basename "$KLIPPER_BIN")" "$(stat -c %s "$KLIPPER_BIN")"
printf 'full-image baseline through the same framing: %s B\n' "$FULL"
printf 'flash cost of writing all %s sectors: %s ms (DS5319 min-max)\n\n' \
    52 "$(grep '^SAVE' "$RUN/server.log" | tail -1 | \
        sed -n 's/.*full_flash_min_ms=\([0-9.]*\).*full_flash_max_ms=\([0-9.]*\).*/\1-\2/p')"
printf '%-26s %4s %4s %5s %8s %7s %9s %7s %13s\n' \
    case skip copy write wire "of full" "saved B" "saved" "flash ms"
printf '%-26s %4s %4s %5s %8s %7s %9s %7s %13s\n' \
    -------------------------- ---- ---- ----- -------- ------- --------- ------- -------------

# Steady state: the destination slot holds the previous release.
measure "1 patched run"          steady "$RUN/churn1.bin"
measure "4 patched runs"         steady "$RUN/churn4.bin"
measure "12 patched runs"        steady "$RUN/churn12.bin"
measure "40 patched runs"        steady "$RUN/churn40.bin"
measure "4 runs, rule 2 off"     steady "$RUN/churn4.bin"  --no-copy
measure "16 B inserted (shift)"  steady "$RUN/shifted.bin"
measure "full image, forced"     steady "$RUN/churn4.bin"  --full
echo
# First update into an erased slot: rule 1 cannot fire, only rule 2 can.
measure "first update, erased"   fresh  "$RUN/churn4.bin"
measure "first update, no copy"  fresh  "$RUN/churn4.bin"  --no-copy
echo
measure_warm "1 run, warm cache"      "$RUN/churn1.bin"
measure_warm "4 runs, warm cache"     "$RUN/churn4.bin"

echo
echo "flash ms is the device cost of erasing and programming the sectors this"
echo "plan writes: copies program the destination too, so rule 2 saves wire"
echo "bytes but no flash time. Rule 1 (already correct) is what saves both."
