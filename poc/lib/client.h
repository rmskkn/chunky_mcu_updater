#ifndef CB_UPDATE_CLIENT_H
#define CB_UPDATE_CLIENT_H
#include "flash.h"
#include "util.h"

/* ECDSA costs are the figures DESIGN.md section 10 budgets, not measurements;
 * the host models a real signature it never actually computes. */
#define ECDSA_SIGN_MS   250.0
#define ECDSA_VERIFY_MS 300.0

/* Device-time instrumentation threaded through a session/boot call, so a
 * caller can print its own timing report afterward instead of the library
 * doing it. sha_bytes/ecdsa_ms accumulate across calls on the same struct,
 * matching the device's total hashing/signing cost so far (per-sector hashes
 * and image_sha256 both run through SHA-256, so one byte counter covers
 * both); the t_* phase timings are reset at the start of the call that fills
 * them. */
typedef struct {
    size_t sha_bytes;
    double ecdsa_ms;
    double t_manifest, t_auth, t_stage, t_persist;  /* client_update_session */
    double t_apply, t_verify;                       /* client_update_boot */
} client_update_stats_t;

/* Opens flash_path, creating it from initial_path (the factory image) on
 * first use. initial_path may be NULL once the device exists. */
void device_open_or_create(flash_t *f, const char *flash_path,
                           const char *initial_path, const uint8_t root_key[32],
                           log_fn log, void *log_ctx);

uint32_t client_app_version(flash_t *f);

/* Runs one whole update session over an already-connected socket: sends the
 * manifest, takes whatever plan the server offers, applies and stages it.
 * stats may be NULL. Returns 0 if a plan was staged, 1 if there was nothing
 * to do (no update offered, or the manifest was rejected). */
int client_update_session(flash_t *f, int fd, const uint8_t dev_key[32],
                          log_fn log, void *log_ctx, client_update_stats_t *stats);

/* Applies whatever the session staged, then checks whether the partition is
 * bootable. stats may be NULL. */
void client_update_boot(flash_t *f, const uint8_t root_key[32],
                        log_fn log, void *log_ctx, client_update_stats_t *stats);

#endif
