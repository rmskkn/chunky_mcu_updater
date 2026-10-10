#!/usr/bin/env bash
# Runtime harness: keeps one server up while you drive the device by hand.
#
#   scripts/lab.sh setup            fresh flash, slot 0 provisioned with v1
#   scripts/lab.sh server           run the server in the foreground
#   scripts/lab.sh release N [RUNS] publish version N, RUNS patched runs (default 4)
#   scripts/lab.sh session          run one update session
#   scripts/lab.sh boot|confirm|unroll|status
#   scripts/lab.sh manifest|map     inspect the flash
#
# Anything else is passed straight to cb-client.
source "$(dirname "$0")/common.sh"
need_build

cmd="${1:-help}"; shift || true

case "$cmd" in
setup)
    need_reference
    fresh_run_dir
    cp "$KLIPPER_BIN" "$RUN/base.bin"
    cp "$RUN/base.bin" "$IMAGES/v1.bin"
    "$CB_FLASH" init "$FLASH"
    "$CB_FLASH" provision "$FLASH" 0 "$IMAGES/v1.bin" 1
    cat <<TXT

ready. two terminals:
  terminal 1:  $0 server
  terminal 2:  $0 release 2 && $0 session && $0 boot && $0 confirm

the server stays up across sessions; publish a new release and run another
session without restarting it.
TXT
    ;;
server)
    [ -d "$IMAGES" ] || { echo "run '$0 setup' first" >&2; exit 1; }
    exec "$CB_SERVER" --port "$PORT" --images "$IMAGES" --cache "$CACHE" "$@"
    ;;
release)
    ver="${1:?usage: $0 release N [RUNS]}"; runs="${2:-4}"
    [ -f "$RUN/base.bin" ] || { echo "run '$0 setup' first" >&2; exit 1; }
    "$CB_FLASH" mutate "$RUN/base.bin" "$IMAGES/v$ver.bin" "$runs" "$ver"
    echo "published v$ver ($runs patched runs); the server picks it up on the next session"
    ;;
manifest|map)
    "$CB_FLASH" "$cmd" "$FLASH"
    ;;
help|-h|--help)
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    ;;
*)
    exec "$CB_CLIENT" "$cmd" --flash "$FLASH" --port "$PORT" "$@"
    ;;
esac
