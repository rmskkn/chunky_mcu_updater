#include "platform.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

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
    p->fd = open(path, O_RDWR);
    if (p->fd < 0) return -1;
    p->base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (p->base == MAP_FAILED) { close(p->fd); return -1; }
    return 0;
}

void plat_flash_close(plat_flash_t *p)
{
    struct stat st;
    fstat(p->fd, &st);
    msync(p->base, (size_t)st.st_size, MS_SYNC);
    munmap(p->base, (size_t)st.st_size);
    close(p->fd);
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
