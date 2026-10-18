#ifndef CB_PLATFORM_POSIX_H
#define CB_PLATFORM_POSIX_H
#include "platform_io.h"

/* This backend's own TCP constructors. Not part of the generic seam in
 * middleware/platform_io.h: connecting, listening and accepting are properties of
 * TCP, not of every transport a port might use (UART just opens a port;
 * CAN has no notion of either). A CLI entry point under src/ -- which
 * already links platform_posix.o by name -- includes this directly. */
plat_io_t *plat_io_tcp_listen(int port);             /* NULL on failure */
plat_io_t *plat_io_tcp_accept(plat_io_t *listener);  /* NULL on failure; blocks */
plat_io_t *plat_io_tcp_connect(const char *host, int port);  /* NULL on failure */

#endif
