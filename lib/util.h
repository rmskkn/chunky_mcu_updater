#ifndef CB_UTIL_H
#define CB_UTIL_H
#include <stdint.h>
#include <stddef.h>

/* What every layer shares: CRC-32, error text, and a log sink. No I/O,
 * clock or allocation; those are the host's (middleware/host.h). The PoC
 * assumes a little-endian host; wire structs are memcpy'd as-is. */

uint32_t crc32_buf(const void *buf, size_t len);

/* Error text a failing call leaves for its caller. Nothing in this code
 * exits on an error: every fallible call returns one, and only a CLI's
 * main() decides what a failure means for the process. */
/* Room for a path and a hint on the host; a small MCU builds with
 * -DCB_ERR_LEN=64 or so, since flash_t carries one. Text is truncated, never
 * overrun. */
#ifndef CB_ERR_LEN
#define CB_ERR_LEN 512
#endif
typedef struct { char msg[CB_ERR_LEN]; } cb_err_t;
/* Formats into e (if non-NULL) and returns -1, so a failing path reads
 * `return err_set(e, "...", ...)`. */
int      err_set(cb_err_t *e, const char *fmt, ...);

/* Optional sink for an informational line a library function wants to
 * report; NULL means "say nothing". Lets the protocol/device logic in
 * lib/device.c and middleware/server_session.c stay free of printf, so a
 * caller other than the CLI binaries (a test, say) can capture or drop the
 * narration. */
typedef void (*log_fn)(void *ctx, const char *line);
void     log_fmt(log_fn fn, void *ctx, const char *fmt, ...);

#endif
