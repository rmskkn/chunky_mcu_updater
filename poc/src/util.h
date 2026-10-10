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

#endif
