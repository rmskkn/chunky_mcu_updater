#include "platform.h"
#include "platform_posix.h"
#include "flash.h"
#include "util.h"
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/socket.h>

int plat_flash_create(const char *path, size_t size, uint8_t fill)
{
    /* One-shot fill buffer, sized to the one flash geometry this build
     * targets rather than malloc'd to an argument the caller always passes
     * as FLASH_SIZE anyway. */
    static uint8_t buf[FLASH_SIZE];
    if (size > sizeof buf) return -1;
    memset(buf, fill, size);
    return write_file(path, buf, size);
}

int plat_flash_open(plat_flash_t *p, const char *path, size_t size)
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    p->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p->base == MAP_FAILED) { close(fd); return -1; }
    p->handle = fd;
    return 0;
}

void plat_flash_close(plat_flash_t *p)
{
    struct stat st;
    fstat((int)p->handle, &st);
    msync(p->base, (size_t)st.st_size, MS_SYNC);
    munmap(p->base, (size_t)st.st_size);
    close((int)p->handle);
}

static void sync_range(plat_flash_t *p, size_t off, size_t len)
{
    size_t start = off & ~(size_t)4095;
    msync(p->base + start, len + (off - start), MS_SYNC);
}

const uint8_t *plat_flash_read(plat_flash_t *p, size_t off)
{
    return p->base + off;
}

void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len)
{
    memcpy(p->base + off, buf, len);
    sync_range(p, off, len);
}

void plat_flash_erase(plat_flash_t *p, size_t off, size_t len)
{
    memset(p->base + off, 0xFF, len);
    sync_range(p, off, len);
}

struct plat_thread {
    pthread_t tid;
    void    (*fn)(void *arg);
    void     *arg;
};

/* client.c never has more than one flash job in flight (flash_worker's own
 * comment: one controller, one job); the second slot is margin for the
 * brief window where the previous job is joined and a new one started. A
 * static pool, not malloc -- the count of threads this device ever needs
 * live at once is fixed and small. */
#define PLAT_THREAD_POOL 2
static struct plat_thread thread_pool[PLAT_THREAD_POOL];
static int thread_used[PLAT_THREAD_POOL];

static void *thread_entry(void *t)
{
    struct plat_thread *th = t;
    th->fn(th->arg);
    return NULL;
}

plat_thread_t *plat_thread_start(void (*fn)(void *arg), void *arg)
{
    for (int i = 0; i < PLAT_THREAD_POOL; i++) {
        if (thread_used[i]) continue;
        struct plat_thread *t = &thread_pool[i];
        t->fn = fn;
        t->arg = arg;
        if (pthread_create(&t->tid, NULL, thread_entry, t)) return NULL;
        thread_used[i] = 1;
        return t;
    }
    return NULL;   /* pool exhausted */
}

void plat_thread_join(plat_thread_t *t)
{
    pthread_join(t->tid, NULL);
    thread_used[t - thread_pool] = 0;
}

struct plat_mutex { pthread_mutex_t m; };

/* One io_lock per update session (client.c), never nested or concurrent
 * across sessions in this CLI; one slot of margin, same reasoning as the
 * thread pool above. */
#define PLAT_MUTEX_POOL 2
static struct plat_mutex mutex_pool[PLAT_MUTEX_POOL];
static int mutex_used[PLAT_MUTEX_POOL];

plat_mutex_t *plat_mutex_create(void)
{
    for (int i = 0; i < PLAT_MUTEX_POOL; i++) {
        if (mutex_used[i]) continue;
        if (pthread_mutex_init(&mutex_pool[i].m, NULL)) return NULL;
        mutex_used[i] = 1;
        return &mutex_pool[i];
    }
    return NULL;
}

void plat_mutex_destroy(plat_mutex_t *m)
{
    pthread_mutex_destroy(&m->m);
    mutex_used[m - mutex_pool] = 0;
}

void plat_mutex_lock(plat_mutex_t *m)   { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex_t *m) { pthread_mutex_unlock(&m->m); }

/* Generic byte-stream seam (lib/platform_io.h). Wraps a socket fd; frame.c
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
