#include "platform.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>

int plat_flash_create(const char *path, size_t size, uint8_t fill)
{
    uint8_t *buf = malloc(size);
    memset(buf, fill, size);
    int rc = write_file(path, buf, size);
    free(buf);
    return rc;
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

static void *thread_entry(void *t)
{
    struct plat_thread *th = t;
    th->fn(th->arg);
    return NULL;
}

plat_thread_t *plat_thread_start(void (*fn)(void *arg), void *arg)
{
    struct plat_thread *t = malloc(sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (pthread_create(&t->tid, NULL, thread_entry, t)) { free(t); return NULL; }
    return t;
}

void plat_thread_join(plat_thread_t *t)
{
    pthread_join(t->tid, NULL);
    free(t);
}

struct plat_mutex { pthread_mutex_t m; };

plat_mutex_t *plat_mutex_create(void)
{
    struct plat_mutex *m = malloc(sizeof *m);
    if (m && pthread_mutex_init(&m->m, NULL)) { free(m); return NULL; }
    return m;
}

void plat_mutex_destroy(plat_mutex_t *m)
{
    pthread_mutex_destroy(&m->m);
    free(m);
}

void plat_mutex_lock(plat_mutex_t *m)   { pthread_mutex_lock(&m->m); }
void plat_mutex_unlock(plat_mutex_t *m) { pthread_mutex_unlock(&m->m); }
