#ifndef CB_PLATFORM_IO_H
#define CB_PLATFORM_IO_H
#include <stddef.h>

/* Byte-stream transport seam. frame.c frames messages over whatever this
 * backs -- TCP, UART, a USB CDC pipe, anything that moves bytes in order.
 * Opaque, so no socket, UART peripheral, or other OS/hardware handle type
 * names itself in lib/ or middleware/.
 *
 * Creating one of these is a property of the transport (TCP needs a host
 * and port; UART needs a baud rate; CAN needs none of that), not something
 * every backend can expose the same way, so it has no function here. The
 * posix backend's own constructors are in platform/platform_posix.h; a CLI
 * entry point under src/ calls those directly. */
typedef struct plat_io plat_io_t;

/* Reads/writes exactly len bytes. 0 on success, -1 on error or a closed
 * link (never a short transfer -- the caller cannot tell those apart). */
int  plat_io_read(plat_io_t *io, void *buf, size_t len);
int  plat_io_write(plat_io_t *io, const void *buf, size_t len);
void plat_io_close(plat_io_t *io);

#endif
