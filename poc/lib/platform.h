#ifndef CB_PLATFORM_H
#define CB_PLATFORM_H
#include <stddef.h>
#include <stdint.h>

/* The seam between the portable update logic in lib/ and the host it runs on.
 * Nothing here names an OS type. platform/platform_posix.c is the host
 * backend; an MCU port supplies its own file in platform/ with the same
 * functions. */

/* Flash IO. base maps the whole part for reads; handle is the backend's own. */
typedef struct {
    uint8_t  *base;
    intptr_t  handle;
} plat_flash_t;

int  plat_flash_open(plat_flash_t *p, const char *path, size_t size);
void plat_flash_close(plat_flash_t *p);

const uint8_t *plat_flash_read(plat_flash_t *p, size_t off);
void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len);
void plat_flash_erase(plat_flash_t *p, size_t off, size_t len);
int plat_flash_create(const char *path, size_t size, uint8_t fill);

/* Threads and a mutex, for the device's flash worker. Opaque so the header
 * carries no OS type; an MCU port can back them with RTOS tasks, or with a
 * call that runs fn inline on start and makes join a no-op. */
typedef struct plat_thread plat_thread_t;
typedef struct plat_mutex  plat_mutex_t;

/* NULL if the thread could not be started. */
plat_thread_t *plat_thread_start(void (*fn)(void *arg), void *arg);
/* Waits for fn to return and releases t. */
void plat_thread_join(plat_thread_t *t);

/* NULL on failure. */
plat_mutex_t *plat_mutex_create(void);
void plat_mutex_destroy(plat_mutex_t *m);
void plat_mutex_lock(plat_mutex_t *m);
void plat_mutex_unlock(plat_mutex_t *m);

#endif
