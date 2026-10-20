#ifndef CB_DEVICE_H
#define CB_DEVICE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "image.h"
#include "manifest.h"
#include "protocol.h"
#include "stats.h"
#include "util.h"

/* The device's update session (DESIGN.md sections 4, 6), and the
 * application's confirm (section 7). The bootloader's apply/verify step is
 * bootloader.h. No transport, no threads, no printf. The session is driven
 * one message at a time: the caller receives a message however its link
 * delivers one and feeds it in; the core does the flash work for it inline
 * and hands any reply to the send callback before it returns. That runs the
 * same under an RTOS or in a superloop. */

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
    const uint8_t  *root_key;        /* verifies the plan's release signature */
    device_send_fn  send;
    void           *send_ctx;
    log_fn          log;
    void           *log_ctx;
    device_stats_t *stats;
    device_stats_t  no_stats;
    int             state;
    uint8_t         device_id[16];
    int             active_slot;     /* loaded at session start; never changes mid-session */
    int             pending_slot;    /* OTHER_SLOT(active_slot) once PLAN_BEGIN arrives */
    int             n_sectors;       /* the plan's image_len / SECTOR_SIZE, once PLAN_BEGIN arrives */
    image_desc_t    desc;            /* the signed descriptor PLAN_BEGIN carried, rebuilt whole */
    size_t          table_got;       /* PLAN_TABLE bytes received so far */
    int             desc_seen;       /* the descriptor sector's step has arrived: only PLAN_END may follow */
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
                         const uint8_t root_key[32], device_send_fn send, void *send_ctx,
                         log_fn log, void *log_ctx, device_stats_t *stats);

/* Feeds the next message from the server; type < 0 means the link closed.
 * len is the payload length as received, and is checked. Returns
 * DEV_PENDING while the session wants another message, then one of the
 * other DEV_* codes once it is over. */
int device_session_feed(device_session_t *s, int type, const uint8_t *payload, size_t len);

/* The application's "I reached a working state and talked to the server"
 * call (DESIGN.md section 7, "application duties on a trial boot"): only
 * valid while plan_state == PLAN_TRIAL. Promotes pending_slot to
 * active_slot, clears the attempt log. 0, or -1 with the reason in
 * f->err -- including "no trial in progress," which is a caller mistake,
 * not a flash fault, but still reported rather than silently ignored. */
int  device_confirm(flash_t *f, log_fn log, void *log_ctx);

#endif
