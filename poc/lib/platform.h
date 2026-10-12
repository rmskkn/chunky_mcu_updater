#ifndef CB_PLATFORM_H
#define CB_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *base;
    int      fd;
} plat_flash_t;

int  plat_flash_open(plat_flash_t *p, const char *path, size_t size);
void plat_flash_close(plat_flash_t *p);

const uint8_t *plat_flash_read(plat_flash_t *p, size_t off);
void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len);
void plat_flash_erase(plat_flash_t *p, size_t off, size_t len);
int plat_flash_create(const char *path, size_t size, uint8_t fill);

#endif
