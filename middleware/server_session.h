#ifndef CB_SERVER_SESSION_H
#define CB_SERVER_SESSION_H
#include "util.h"
#include "platform_io.h"
#include <stdint.h>

/* No compression counters (format2 has no layer-0 compression, SPEC.md 10) and no plan_t (format2's plan is a flat byte stream); erase/copy/write are step counts straight off the plan. */
typedef struct {
    uint32_t ver;
    int      erase, copy, write;
    uint64_t tx_bytes, rx_bytes;
} server_session_result_t;

/* The server-side session (SPEC.md; MIGRATION_FORMAT2.md): HELLO, the device's manifest (received but not used to plan, SPEC.md 7.2), diffs the target release against it, then streams the plan. Returns 1 and fills *out if a plan was streamed, 0 if the session ended before that, or -1 (reason in *err) on the server's own fault. abort_before_commit (test-only) sends ABORT instead of COMMIT right after the device's ACK. */
int server_update_session(plat_io_t *io, const char *image_path,
                           int abort_before_commit,
                           log_fn log, void *log_ctx,
                           server_session_result_t *out, cb_err_t *err);

#endif
