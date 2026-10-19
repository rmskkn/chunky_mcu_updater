/* The host's side of the things lib/ never does itself: files, randomness,
 * a clock, the environment. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

void hexdump_line(const uint8_t *p, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hx[p[i] >> 4];
        out[i * 2 + 1] = hx[p[i] & 0xF];
    }
    out[len * 2] = 0;
}

uint8_t *read_file(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(n ? (size_t)n : 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

int write_file(const char *path, const void *buf, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t n = fwrite(buf, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int machine_output(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("CB_MACHINE") != NULL;
    return cached;
}

double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000.0 + (double)t.tv_nsec / 1e6;
}

void rnd_bytes(uint8_t *out, size_t len)
{
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { size_t n = fread(out, 1, len, f); fclose(f); if (n == len) return; }
    for (size_t i = 0; i < len; i++) out[i] = (uint8_t)(rand() >> 7);
}
