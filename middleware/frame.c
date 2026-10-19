#include "frame.h"
#include "util.h"
#include <string.h>

void link_init(link_t *l, plat_io_t *io)
{
    memset(l, 0, sizeof *l);
    l->io = io;
}

int frame_send(link_t *l, uint8_t type, const void *payload, size_t len)
{
    uint8_t buf[FRAME_OVERHEAD + FRAME_MAX_PAYLOAD];

    if (len > FRAME_MAX_PAYLOAD) return -1;
    buf[0] = FRAME_MAGIC0; buf[1] = FRAME_MAGIC1;
    buf[2] = (uint8_t)(len & 0xFF);
    buf[3] = (uint8_t)(len >> 8);
    buf[4] = type;
    buf[5] = l->tx_seq++;
    if (len) memcpy(buf + FRAME_HDR_LEN, payload, len);
    uint32_t c = crc32_buf(buf + 2, 4 + len);
    memcpy(buf + FRAME_HDR_LEN + len, &c, 4);

    size_t total = FRAME_OVERHEAD + len;
    l->tx_bytes += total;
    l->tx_frames++;
    return plat_io_write(l->io, buf, total);
}

int frame_recv(link_t *l, uint8_t *payload, size_t *len)
{
    uint8_t hdr[FRAME_HDR_LEN], crc[4];

    if (plat_io_read(l->io, hdr, FRAME_HDR_LEN) < 0) return FRAME_EOF;
    if (hdr[0] != FRAME_MAGIC0 || hdr[1] != FRAME_MAGIC1) return FRAME_BAD;
    size_t n = (size_t)hdr[2] | ((size_t)hdr[3] << 8);
    if (n > FRAME_MAX_PAYLOAD) return FRAME_BAD;
    if (n && plat_io_read(l->io, payload, n) < 0) return FRAME_EOF;
    if (plat_io_read(l->io, crc, 4) < 0) return FRAME_EOF;

    uint8_t chk[4 + FRAME_MAX_PAYLOAD];
    memcpy(chk, hdr + 2, 4);
    if (n) memcpy(chk + 4, payload, n);
    uint32_t want;
    memcpy(&want, crc, 4);
    if (crc32_buf(chk, 4 + n) != want) return FRAME_BAD;
    /* Each side numbers its frames from 0 per link; a gap or a repeat means
     * a frame was lost or replayed. */
    if (hdr[5] != l->rx_seq) return FRAME_BAD;
    l->rx_seq++;

    l->rx_bytes += FRAME_OVERHEAD + n;
    l->rx_frames++;
    *len = n;
    return hdr[4];
}

int frame_expect(link_t *l, uint8_t type, uint8_t *payload, size_t *len)
{
    int t = frame_recv(l, payload, len);
    if (t < 0) return t;
    return t == type ? t : FRAME_BAD;
}
