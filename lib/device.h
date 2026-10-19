#ifndef CB_DEVICE_H
#define CB_DEVICE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "manifest.h"
#include "protocol.h"
#include "util.h"

/* The device's functional core: the update session (DESIGN.md sections 4,
 * 6) and the bootloader's apply/verify step (sections 7, 8). No transport,
 * no threads, no printf. The session is driven one message at a time: the
 * caller receives a message however its link delivers one and feeds it in;
 * the core does the flash work for it inline and hands any reply to the
 * send callback before it returns. That runs the same under an RTOS, in a
 * superloop, or in a bootloader. */

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
    double t_apply, t_verify;                       /* device_boot */
} device_stats_t;

/* Sends one message to the server. Returns 0, or -1 if the link failed. */
typedef int (*device_send_fn)(void *ctx, uint8_t type, const void *payload, size_t len);

/* What a session call returns. */
enum {
    DEV_PENDING = 0,  /* feed the next message */
    DEV_STAGED,       /* a plan was persisted; armed iff the server sent COMMIT */
    DEV_NO_UPDATE,    /* nothing offered, or the manifest was rejected */
    DEV_ERROR         /* protocol or input fault; the reason is in error */
};

typedef struct {
    char error[96];   /* why the last call returned DEV_ERROR */

    /* Everything below is the core's own. */
    flash_t        *f;
    const uint8_t  *dev_key;
    device_send_fn  send;
    void           *send_ctx;
    log_fn          log;
    void           *log_ctx;
    device_stats_t *stats;
    device_stats_t  no_stats;
    int             state;
    uint8_t         device_id[16];
    uint32_t        generation;      /* of the meta bank the session started on */
    double          t0;
    manifest_t      man;
    msg_manifest_auth_t auth;
    meta_t          np;              /* the plan being staged */
    uint8_t         done[SECTORS_SLOT];
    int             pending_copies;
    uint8_t         enc[SECTOR_SIZE];   /* the encoded sector being assembled */
    uint8_t         dict[SECTOR_SIZE];  /* dst's current content, the LZ4 dictionary */
    uint8_t         sect[SECTOR_SIZE];  /* the decoded sector */
} device_session_t;

/* Starts a session: reads the device's state and sends HELLO. stats may be
 * NULL. Returns DEV_PENDING, or DEV_ERROR. */
int device_session_start(device_session_t *s, flash_t *f, const uint8_t dev_key[32],
                         device_send_fn send, void *send_ctx,
                         log_fn log, void *log_ctx, device_stats_t *stats);

/* Feeds the next message from the server; type < 0 means the link closed.
 * len is the payload length as received, and is checked. Returns
 * DEV_PENDING while the session wants another message, then one of the
 * other DEV_* codes once it is over. */
int device_session_feed(device_session_t *s, int type, const uint8_t *payload, size_t len);

uint32_t device_app_version(flash_t *f);

/* Applies whatever the session staged, then checks whether the partition is
 * bootable. stats may be NULL. 0, or -1 if the result could not be
 * recorded in flash (the reason in f->err); a plan that fails to apply or
 * an image that fails to verify is a recorded outcome, not an error. */
int  device_boot(flash_t *f, const uint8_t root_key[32],
                 log_fn log, void *log_ctx, device_stats_t *stats);

#endif
