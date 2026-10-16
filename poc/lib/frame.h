#ifndef CB_FRAME_H
#define CB_FRAME_H
#include <stdint.h>
#include <stddef.h>
#include "platform_io.h"

/* | 0xA5 0x5A | len:u16 | type:u8 | seq:u8 | payload | crc32:u32 |
 * DESIGN.md section 6: 2 + 2 + 1 + 1 + 4 = 10 bytes of overhead per frame. */
#define FRAME_OVERHEAD    10
#define FRAME_MAX_PAYLOAD 288

typedef struct {
    plat_io_t *io;
    uint8_t  tx_seq;
    uint64_t tx_bytes, rx_bytes;
    uint64_t tx_frames, rx_frames;
} link_t;

void link_init(link_t *l, plat_io_t *io);
int  frame_send(link_t *l, uint8_t type, const void *payload, size_t len);
/* Returns the message type, or -1 on EOF/error. */
int  frame_recv(link_t *l, uint8_t *payload, size_t *len);
int  frame_expect(link_t *l, uint8_t type, uint8_t *payload, size_t *len);

#endif
