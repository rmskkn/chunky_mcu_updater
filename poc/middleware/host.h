#ifndef CB_HOST_H
#define CB_HOST_H
#include <stdint.h>
#include <stddef.h>
#include "util.h"

/* Host services the middleware and the CLIs use and lib/ never does:
 * files, randomness, a clock, the environment. An MCU port has its own or
 * none. */

void     hexdump_line(const uint8_t *p, size_t len, char *out);
/* Whole file into a malloc'd buffer; NULL on failure. */
uint8_t *read_file(const char *path, size_t *len_out);
int      write_file(const char *path, const void *buf, size_t len);
void     rnd_bytes(uint8_t *out, size_t len);

/* Monotonic wall clock in milliseconds, for the phase timers. */
double   now_ms(void);

/* True when CB_MACHINE is set, which turns on the parseable STATS/SAVE/TIME
 * lines the scripts read. Off by default so a run is readable. */
int      machine_output(void);

#endif
