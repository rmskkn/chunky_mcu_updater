#include "platform_posix.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

/* Generic byte-stream seam (middleware/platform_io.h). Wraps a socket fd; frame.c
 * never sees the fd or any socket call, only these three functions. A
 * static pool, not malloc: cb-server holds its listener plus at most one
 * accepted connection at a time (the accept loop is sequential), cb-client
 * holds one; two slots of margin cover both with room to spare. */
struct plat_io { int fd; };
#define PLAT_IO_POOL 4
static struct plat_io io_pool[PLAT_IO_POOL];
static int io_used[PLAT_IO_POOL];

int plat_io_read(plat_io_t *io, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(io->fd, p, len);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

int plat_io_write(plat_io_t *io, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(io->fd, p, len);
        if (n <= 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

void plat_io_close(plat_io_t *io)
{
    if (!io) return;
    close(io->fd);
    io_used[io - io_pool] = 0;
}

/* TCP constructors, this backend's own (platform/platform_posix.h) -- not
 * part of the generic seam, since not every transport connects/listens the
 * way TCP does. */
static plat_io_t *io_wrap(int fd)
{
    for (int i = 0; i < PLAT_IO_POOL; i++) {
        if (io_used[i]) continue;
        io_used[i] = 1;
        io_pool[i].fd = fd;
        return &io_pool[i];
    }
    close(fd);
    return NULL;   /* pool exhausted */
}

plat_io_t *plat_io_tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return NULL; }
    if (listen(fd, 4) < 0) { close(fd); return NULL; }
    return io_wrap(fd);
}

plat_io_t *plat_io_tcp_accept(plat_io_t *listener)
{
    int fd = accept(listener->fd, NULL, NULL);
    return fd < 0 ? NULL : io_wrap(fd);
}

plat_io_t *plat_io_tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return NULL; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return NULL; }
    return io_wrap(fd);
}
