#ifndef CB_STATS_H
#define CB_STATS_H
#include <stddef.h>

/* Device-time instrumentation shared by the update session (device.h) and
 * the bootloader (bootloader.h), so neither includes the other. */

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
 * them. The core has no clock of its own: clock_ms is the caller's, and
 * with it NULL every t_* stays 0. */
typedef struct {
    double (*clock_ms)(void);   /* monotonic milliseconds; may be NULL */
    size_t sha_bytes;
    double ecdsa_ms;
    double t_manifest, t_auth, t_stage, t_persist;  /* device_session_* */
    double t_apply, t_verify;                       /* bootloader_boot */
    size_t verify_bytes;        /* bytes the last bootloader_boot hashed to verify a pending image */
} device_stats_t;

static inline double stats_tick(const device_stats_t *st)
{
    return st->clock_ms ? st->clock_ms() : 0.0;
}

#endif
