#ifndef CB_DEVICE_H
#define CB_DEVICE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_manifest.h"
#include "format2_plan.h"
#include "protocol.h"
#include "stats.h"
#include "util.h"

/* Sends one message to the server. Returns 0, or -1 if the link failed. */
typedef int (*device_send_fn)(void *ctx, uint8_t type, const void *payload, size_t len);

/* What a session call returns. */
enum {
    DEV_PENDING = 0,  /* feed the next message */
    DEV_STAGED,       /* a plan was persisted; armed iff the server sent COMMIT */
    DEV_NO_UPDATE,    /* nothing offered, or the manifest was rejected */
    DEV_ERROR         /* protocol or input fault; the reason is in error */
};

/* The device's update session (SPEC.md; MIGRATION_FORMAT2.md): fed one message at a time; buffers the whole plan body and applies every step live during the session (format2_plan_walk() needs one contiguous buffer), so the bootloader has nothing left to apply. */

/* Worst-case plan size: header plus every sector ERASEd/WRITE-fragmented (COPY is smaller); compile-time-sized since lib/ has no malloc (AGENTS.md), small enough to afford (tens of KB) on this device class. */
#define DEVICE_PLAN_BUF_CAP \
    (CHUNKY_PLAN_HEADER_LEN + (size_t)FORMAT2_BLOCKS * 16u + (size_t)SLOT_SIZE)

/* format2_manifest_build()'s stated bound (lib/format2_manifest.h): 13 B header plus up to 2*FORMAT2_CHUNKS entries of 9 B each. */
#define DEVICE_MANIFEST_BUF_CAP (13u + 2u * (size_t)FORMAT2_CHUNKS * 9u)

typedef struct {
    char error[96];   /* why the last call returned DEV_ERROR */

    /* Everything below is the core's own. */
    flash_t        *f;
    device_send_fn  send;
    void           *send_ctx;
    log_fn          log;
    void           *log_ctx;
    device_stats_t *stats;
    device_stats_t  no_stats;
    int             state;
    uint8_t         device_id[16];
    int             active_slot;      /* loaded at session start; never changes mid-session */
    int             pending_slot;     /* OTHER_SLOT(active_slot) once a plan starts arriving */
    uint32_t        security_floor;   /* meta_t.security_floor at session start; this plan's anti-rollback gate */
    meta_t          np;               /* the plan being staged */
    size_t          plan_got;         /* plan bytes buffered so far */
    uint8_t         plan_buf[DEVICE_PLAN_BUF_CAP];
    uint8_t         scratch[FORMAT2_CHUNK_LEN];  /* format2_plan_walk()'s COPY-source scratch */
    uint8_t         manifest_buf[DEVICE_MANIFEST_BUF_CAP];
} device_session_t;

/* Starts a session: reads device state, sends HELLO then MANIFEST; no challenge round trip and no key parameters (format2 scheme 2 is unsigned). stats may be NULL. Returns DEV_PENDING, or DEV_ERROR. */
int device_session_start(device_session_t *s, flash_t *f,
                          device_send_fn send, void *send_ctx,
                          log_fn log, void *log_ctx, device_stats_t *stats);

/* Feeds the next message from the server; type < 0 means the link closed.
 * len is the payload length as received, and is checked. Returns
 * DEV_PENDING while the session wants another message, then one of the
 * other DEV_* codes once it is over. */
int device_session_feed(device_session_t *s, int type, const uint8_t *payload, size_t len);

/* The application's "I reached a working state and talked to the server" call (DESIGN.md 7): promotes pending_slot to active_slot, clears the attempt log, and raises meta_t.security_floor to the confirmed image's security_counter read back from flash (never trusted from the now-gone session; SPEC.md 12.4: the floor only rises). 0, or -1 (reason in f->err), including a caller mistake like "no trial in progress." */
int device_confirm(flash_t *f, log_fn log, void *log_ctx);

#endif
