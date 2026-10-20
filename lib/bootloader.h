#ifndef CB_BOOTLOADER_H
#define CB_BOOTLOADER_H
#include <stdint.h>
#include "flash.h"
#include "stats.h"
#include "util.h"

/* The bootloader's apply/verify/trial-boot step (DESIGN.md sections 7, 8).
 * It shares nothing with the update session (device.h) but flash and the
 * meta bank: no transport, no protocol messages. */

/* Applies whatever the session staged, then decides whether the result is
 * bootable (DESIGN.md sections 7-8). stats may be NULL. 0, or -1 if the
 * result could not be recorded in flash (the reason in f->err); a plan that
 * fails to apply, an image that fails to verify, or a trial that runs out
 * of attempts is a recorded outcome, not an error.
 *
 * Calling contract (docs/ab_migration.md's open question, resolved this
 * way): the host PoC has no real watchdog or CPU reset, so one call is one
 * simulated boot/reset cycle, and a test drives repeated trial attempts by
 * calling this repeatedly -- the attempt log persists in meta_t across
 * calls, same as it would across real resets. A trial boot that succeeds
 * does *not* auto-confirm; the caller (standing in for the application
 * reaching a working state and talking to the server) must call
 * device_confirm() separately, or call bootloader_boot() again to simulate a
 * reset with no confirm, which consumes another attempt. This is the
 * "closest to the real mechanism" option the migration plan offered,
 * chosen over a single-call simplified resolution because it is what lets
 * a shell-level test (this repo has no unit-test harness in C) exercise
 * the confirm/retry/revert paths with the real cb-client binary: see
 * src/client.c's --boot-only / --confirm-only / --no-confirm flags. */
int  bootloader_boot(flash_t *f, const uint8_t root_key[32],
                     log_fn log, void *log_ctx, device_stats_t *stats);

#endif
