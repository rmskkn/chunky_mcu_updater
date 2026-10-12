#ifndef CB_PLATFORM_H
#define CB_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

/* Backing-store IO for one flash device, isolated from flash.c's page
 * geometry, meta-bank, and journal logic. This is the seam a real target
 * swaps: everything above it calls only plat_flash_*, never a file or
 * memory-map primitive directly. platform_posix.c is the only
 * implementation today -- a host file stands in for the device, read and
 * written through mmap. A bare-metal target adds its own platform_<mcu>.c
 * against the same calls, using its HAL for erase/program and a direct
 * pointer (XIP) for read, instead of this one. */
typedef struct {
    uint8_t *base;
    int      fd;
} plat_flash_t;

int  plat_flash_open(plat_flash_t *p, const char *path, size_t size);
void plat_flash_close(plat_flash_t *p);

/* Zero-copy read: flash is memory-mapped on the host (mmap) and on a real
 * target (XIP), so callers read straight out of the returned pointer
 * rather than through a copying read(). */
const uint8_t *plat_flash_read(plat_flash_t *p, size_t off);

/* Raw half-word write: no erase, no "bits only go 1 -> 0" check. That
 * invariant belongs to flash.c, not the backing store. */
void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len);

void plat_flash_erase(plat_flash_t *p, size_t off, size_t len);

/* Host only: create and zero-fill a file standing in for provisioned
 * flash. Not part of the calls above -- a real target's flash already
 * exists and never calls this. */
int plat_flash_create(const char *path, size_t size, uint8_t fill);

#endif
