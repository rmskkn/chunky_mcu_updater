#ifndef CB_FRAME_H
#define CB_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include "platform_io.h"
#include "protocol.h"

/* Frames protocol.h messages over a plat_io_t; the layout is defined there. */

typedef struct {
    plat_io_t *io;
    uint8_t  tx_seq;
    uint8_t  rx_seq;         /* seq the next received frame must carry */
    uint64_t tx_bytes, rx_bytes;
    uint64_t tx_frames, rx_frames;
} link_t;

void link_init(link_t *l, plat_io_t *io);
/* 0, or -1 if the payload is over FRAME_MAX_PAYLOAD or the link failed. */
int  frame_send(link_t *l, uint8_t type, const void *payload, size_t len);
/* frame_recv/frame_expect results below zero. A bad frame is never fatal
 * here: whatever is on the other end of the link controls these bytes, so
 * the caller ends the session instead. */
#define FRAME_EOF  (-1)   /* link closed or failed */
#define FRAME_BAD  (-2)   /* bad magic, length, CRC or seq, or (frame_expect) type */

/* Returns the message type, or FRAME_EOF / FRAME_BAD. */
int  frame_recv(link_t *l, uint8_t *payload, size_t *len);
int  frame_expect(link_t *l, uint8_t type, uint8_t *payload, size_t *len);

#endif
