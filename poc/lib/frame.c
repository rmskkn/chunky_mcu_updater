#include "frame.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

void link_init(link_t *l, int fd)
{
    memset(l, 0, sizeof *l);
    l->fd = fd;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n <= 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

int frame_send(link_t *l, uint8_t type, const void *payload, size_t len)
{
    uint8_t buf[FRAME_OVERHEAD + FRAME_MAX_PAYLOAD];

    if (len > FRAME_MAX_PAYLOAD) die("frame: payload %zu over cap", len);
    buf[0] = 0xA5; buf[1] = 0x5A;
    buf[2] = (uint8_t)(len & 0xFF);
    buf[3] = (uint8_t)(len >> 8);
    buf[4] = type;
    buf[5] = l->tx_seq++;
    if (len) memcpy(buf + 6, payload, len);
    uint32_t c = crc32_buf(buf + 2, 4 + len);
    memcpy(buf + 6 + len, &c, 4);

    size_t total = FRAME_OVERHEAD + len;
    l->tx_bytes += total;
    l->tx_frames++;
    return write_all(l->fd, buf, total);
}

int frame_recv(link_t *l, uint8_t *payload, size_t *len)
{
    uint8_t hdr[6], crc[4];

    if (read_all(l->fd, hdr, 6) < 0) return -1;
    if (hdr[0] != 0xA5 || hdr[1] != 0x5A) die("frame: bad magic");
    size_t n = (size_t)hdr[2] | ((size_t)hdr[3] << 8);
    if (n > FRAME_MAX_PAYLOAD) die("frame: length %zu over cap", n);
    if (n && read_all(l->fd, payload, n) < 0) return -1;
    if (read_all(l->fd, crc, 4) < 0) return -1;

    uint8_t chk[4 + FRAME_MAX_PAYLOAD];
    memcpy(chk, hdr + 2, 4);
    if (n) memcpy(chk + 4, payload, n);
    uint32_t want;
    memcpy(&want, crc, 4);
    if (crc32_buf(chk, 4 + n) != want) die("frame: crc32 mismatch");

    l->rx_bytes += FRAME_OVERHEAD + n;
    l->rx_frames++;
    *len = n;
    return hdr[4];
}

int frame_expect(link_t *l, uint8_t type, uint8_t *payload, size_t *len)
{
    int t = frame_recv(l, payload, len);
    if (t < 0) return -1;
    if (t != type) die("protocol: expected message %u, got %d", type, t);
    return t;
}

int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    if (listen(fd, 4) < 0) { close(fd); return -1; }
    return fd;
}

int tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}
