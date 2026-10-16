#ifndef CB_UTIL_H
#define CB_UTIL_H
#include <stdint.h>
#include <stddef.h>

/* Host PoC assumes a little-endian host; wire structs are memcpy'd as-is. */

uint32_t crc32_buf(const void *buf, size_t len);
void     hexdump_line(const uint8_t *p, size_t len, char *out);
uint8_t *read_file(const char *path, size_t *len_out);
int      write_file(const char *path, const void *buf, size_t len);
void     die(const char *fmt, ...);
void     rnd_bytes(uint8_t *out, size_t len);

/* Monotonic wall clock in milliseconds, for the phase timers. */
double   now_ms(void);

/* True when CB_MACHINE is set, which turns on the parseable STATS/SAVE/TIME
 * lines the scripts read. Off by default so a run is readable. */
int      machine_output(void);

/* Optional sink for an informational line a library function wants to
 * report; NULL means "say nothing". Lets the protocol/device logic in
 * client.c and update_server.c stay free of printf, so a caller other
 * than the CLI binaries (a test, say) can capture or drop the narration. */
typedef void (*log_fn)(void *ctx, const char *line);
void     log_fmt(log_fn fn, void *ctx, const char *fmt, ...);

#endif
