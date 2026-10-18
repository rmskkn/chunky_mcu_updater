#include "util.h"
#include <stdio.h>
#include <stdarg.h>

static uint32_t crc_table[256];
static int crc_init;

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_init = 1;
}

uint32_t crc32_buf(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    if (!crc_init) crc32_init();
    while (len--) c = crc_table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

int err_set(cb_err_t *e, const char *fmt, ...)
{
    if (e) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(e->msg, sizeof e->msg, fmt, ap);
        va_end(ap);
    }
    return -1;
}

void log_fmt(log_fn fn, void *ctx, const char *fmt, ...)
{
    if (!fn) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fn(ctx, buf);
}
