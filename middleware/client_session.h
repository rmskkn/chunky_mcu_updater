#ifndef CB_CLIENT_SESSION_H
#define CB_CLIENT_SESSION_H
#include "device.h"
#include "platform_io.h"

/* Runs one whole update session over an already-connected link: frames
 * messages in and out and drives lib/device.c one message at a time.
 * No key parameters: format2 scheme 2 is unsigned. stats may be NULL. Returns 0 if a plan was staged, 1 if nothing was offered, or -1 (reason in *err) on a protocol, link or flash fault. */
int client_update_session(flash_t *f, plat_io_t *io, log_fn log, void *log_ctx,
                          device_stats_t *stats, cb_err_t *err);

#endif
