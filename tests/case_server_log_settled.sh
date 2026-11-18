#!/usr/bin/env bash
# update() returns only once the server has ended the session, so a case that
# reads server.log after it reads that session's lines, not the previous
# one's. The server here holds each session's end, after the device is done,
# until the case opens a FIFO: a loaded host taken to the limit. update() must
# still be running, waiting for the server, when the case opens it.
source "$(dirname "$0")/common.sh"
need_build
test_init server_log_settled

gen_random_bin "$BASE" $PAYLOAD_MAX 501
cp "$BASE" "$IMAGE"; scramble_sector "$IMAGE" $((SECTORS / 2)) 5151
mkfifo "$RUN/gate"
IDLE_MARK="$RUN/server_idle.waiting"

trap 'exec 4>&-; server_stop' EXIT
server_start --hold-session-end "$RUN/gate"

update >"$RUN/update.out" 2>&1 &
upd=$!
# update() either returned or saw the held session and waits for it
until ! kill -0 "$upd" 2>/dev/null || [ -e "$IDLE_MARK" ]; do sleep 0.01; done
if ! kill -0 "$upd" 2>/dev/null; then
    wait "$upd" || fail "update" "update() failed; see $RUN/update.out"
    fail "early return" "update() returned while the server still held the session"
fi

# Release the server. A read-write open of a FIFO never blocks, so a server
# that died before opening its end cannot hang the case.
exec 4<>"$RUN/gate"
wait "$upd"
exec 4>&-
out="$(cat "$RUN/update.out")"
assert_contains "$out" "booting version 2" "the update boots v2"
grep -q '^server: session end' "$RUN/server.log" || fail "session end" "update() returned before the server ended the session"
grep -qE '^sectors: .* 2 sent,' "$RUN/server.log" || fail "report" "the session's report is not in server.log"
[ "$(compress_count raw)" -ge 1 ] || fail "compress line" "the session's compress counts are not in server.log"

ok "server_log_settled"
