#ifndef CB_FLASH_FILE_H
#define CB_FLASH_FILE_H
#include <stdint.h>
#include "flash.h"
#include "util.h"

/* Host middleware: the simulated part as a file, mmap'd into a plat_flash_t.
 * Between the core (lib/), which works on a part that is already open, and
 * the CLI entry points (src/), which only know paths. An MCU port has no
 * equivalent: its part sits at fixed addresses and is never created. */

/* Creates a blank part at path: erased, with a bootloader marker and a
 * provisioned identity unit, as a factory programmer would leave it. */
int  flash_file_create(const char *path);

/* -1 with errno set if path cannot be opened or mapped. */
int  flash_file_open(flash_t *f, const char *path);
void flash_file_close(flash_t *f);

/* Opens flash_path, creating it from initial_path (the factory image) on
 * first use. initial_path may be NULL once the device exists. 0, or -1
 * with the reason in *err. */
int  device_open_or_create(flash_t *f, const char *flash_path,
                           const char *initial_path, const uint8_t root_key[32],
                           log_fn log, void *log_ctx, cb_err_t *err);

#endif
