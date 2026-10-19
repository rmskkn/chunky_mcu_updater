#ifndef CB_FLASH_PORT_H
#define CB_FLASH_PORT_H
#include <stddef.h>

/* The flash the core runs on, as the core needs it, and nothing more. lib/
 * declares the contract; a port implements it (the host's is the mmap'd
 * file in middleware/flash_file.c; an MCU's drives its flash controller).
 * The handle is opaque, so no address, file descriptor or peripheral type
 * reaches lib/.
 *
 * Reads copy into the caller's buffer rather than return a pointer: only
 * memory-mapped flash has one to give, and a QSPI or external SPI part, or a
 * read that must survive an ECC fault, needs a copy. Offsets are bytes from
 * the start of the part, and the core keeps every call inside FLASH_SIZE and
 * inside the part's program rules before it makes it. Opening, creating and
 * closing a part are where it lives, not what the core does, so they are
 * not here. */
typedef struct plat_flash plat_flash_t;

void plat_flash_read(plat_flash_t *p, size_t off, void *buf, size_t len);
void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len);
void plat_flash_erase(plat_flash_t *p, size_t off, size_t len);

#endif
