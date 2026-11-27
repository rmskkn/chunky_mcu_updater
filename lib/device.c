#include "device.h"
#include "format2_image.h"
#include "chunky_format.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Session states: which message comes next; S_OFFER covers both "nothing offered" and the first PLAN fragment. */
enum { S_OFFER, S_PLAN, S_COMMIT, S_DONE };

static int fail(device_session_t *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->error, sizeof s->error, fmt, ap);
    va_end(ap);
    s->state = S_DONE;
    return DEV_ERROR;
}

static void reply(device_session_t *s, uint8_t type, const void *payload, size_t len)
{
    /* A failed send needs no handling of its own: the reply it carried
     * never arrives, so the next feed is the link closing. */
    (void)s->send(s->send_ctx, type, payload, len);
}

static void ack(device_session_t *s, uint8_t code)
{
    msg2_step_ack_t a = { code };
    reply(s, code == 0 ? MSG2_STEP_ACK : MSG2_STEP_NAK, &a, sizeof a);
}

int device_session_start(device_session_t *s, flash_t *f,
                          device_send_fn send_fn, void *send_ctx,
                          log_fn log, void *log_ctx, device_stats_t *stats)
{
    memset(s, 0, sizeof *s);
    s->f = f;
    s->send = send_fn;
    s->send_ctx = send_ctx;
    s->log = log;
    s->log_ctx = log_ctx;
    s->stats = stats ? stats : &s->no_stats;
    s->stats->t_manifest = s->stats->t_auth = s->stats->t_stage = s->stats->t_persist = 0;

    identity_t id;
    if (identity_get(f, &id) < 0) return fail(s, "%s", f->err.msg);
    memcpy(s->device_id, id.device_id, sizeof s->device_id);

    meta_t m;
    meta_load(f, &m);
    s->active_slot = m.active_slot;
    s->security_floor = m.security_floor;

    msg2_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, s->device_id, 16);
    h.geometry_id = format2_geometry_id();
    h.product_id  = CB_PRODUCT_ID;
    h.active_slot = (slot_id_t)s->active_slot;
    h.last_result = (uint8_t)m.last_result;
    {
        /* running_version/running_sha256 stay 0/all-zero if the active slot doesn't hold a verifying image (floor 0: a report, not a gate). */
        uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
        chunky_descriptor_body_view_t body;
        if (format2_image_verify_flash(f, s->active_slot, 0u, desc_raw, &body) == FORMAT2_OK) {
            h.running_version = chunky_descriptor_body_version(body);
            format2_image_hash_flash(f, s->active_slot, h.running_sha256);
        }
    }
    reply(s, MSG2_HELLO, &h, sizeof h);

    double t0 = stats_tick(s->stats);
    size_t mlen = format2_manifest_build(f, s->active_slot, OTHER_SLOT(s->active_slot),
                                         s->manifest_buf, sizeof s->manifest_buf);
    s->stats->t_manifest = stats_tick(s->stats) - t0;
    if (mlen == 0) return fail(s, "format2: manifest build failed (out_cap too small)");
    s->stats->sha_bytes += (size_t)2 * (size_t)FORMAT2_CHUNKS * (size_t)FORMAT2_CHUNK_LEN;
    /* mlen can exceed FRAME_MAX_PAYLOAD, so this fragments like a PLAN body (off:u16, len:u16, PLAN2_FRAG_HDR), closed by MSG2_MANIFEST_END. */
    {
        size_t off = 0;
        const size_t max_body = FRAME_MAX_PAYLOAD - PLAN2_FRAG_HDR;
        while (off < mlen) {
            uint8_t frag[FRAME_MAX_PAYLOAD];
            uint16_t o16 = (uint16_t)off;
            uint16_t flen = (uint16_t)(mlen - off < max_body ? mlen - off : max_body);
            memcpy(frag, &o16, 2);
            memcpy(frag + 2, &flen, 2);
            memcpy(frag + PLAN2_FRAG_HDR, s->manifest_buf + off, flen);
            reply(s, MSG2_MANIFEST, frag, PLAN2_FRAG_HDR + flen);
            off += flen;
        }
        reply(s, MSG2_MANIFEST_END, NULL, 0);
    }
    log_fmt(s->log, s->log_ctx, "device: HELLO + manifest sent (%zu B)", mlen);

    s->state = S_OFFER;
    return DEV_PENDING;
}

/* Buffers one MSG2_PLAN fragment: off:u16, len:u16, then the bytes (PLAN2_FRAG_HDR, lib/protocol.h). */
static int on_plan_frag(device_session_t *s, const uint8_t *pay, size_t n)
{
    uint16_t off, len;
    if (n < PLAN2_FRAG_HDR) return fail(s, "short PLAN fragment (%zu B)", n);
    memcpy(&off, pay, 2);
    memcpy(&len, pay + 2, 2);
    if (off != s->plan_got || (size_t)off + len > sizeof s->plan_buf || len > n - PLAN2_FRAG_HDR)
        return fail(s, "bad PLAN fragment (off=%u len=%u, have %zu B)", off, len, s->plan_got);
    memcpy(s->plan_buf + off, pay + PLAN2_FRAG_HDR, len);
    s->plan_got += len;
    s->state = S_PLAN;
    return DEV_PENDING;
}

static int on_plan_end(device_session_t *s)
{
    chunky_descriptor_body_view_t body;
    chunky_plan_header_view_t h;
    format2_apply_ctx_t actx;
    format2_err_t e;
    uint32_t step_count;
    int rc;

    e = format2_plan_begin(s->plan_buf, s->plan_got, format2_geometry_id(), CB_PRODUCT_ID,
                           s->security_floor, (uint32_t)FORMAT2_IMAGE_LEN, &body);
    if (e != FORMAT2_OK) {
        ack(s, (uint8_t)e);
        return fail(s, "format2: plan refused: %s", format2_err_str(e));
    }

    /* format2_plan_begin() already confirmed the header is long enough; this view is defensive, not reachable. */
    if (chunky_plan_header_view(s->plan_buf, s->plan_got, &h) != 0)
        return fail(s, "format2: plan header too short");
    step_count = chunky_plan_header_step_count(h);

    s->pending_slot = OTHER_SLOT(s->active_slot);
    actx.f = s->f;
    actx.dfu_slot = s->pending_slot;
    actx.active_slot = s->active_slot;

    double t0 = stats_tick(s->stats);
    rc = format2_plan_walk(s->plan_buf, s->plan_got, step_count,
                           (uint32_t)FORMAT2_BLOCKS, (uint32_t)FORMAT2_CHUNKS,
                           (uint16_t)FORMAT2_CHUNK_LEN,
                           format2_read_active, &actx, s->scratch,
                           format2_apply_step, &actx);
    s->stats->t_stage = stats_tick(s->stats) - t0;
    s->stats->sha_bytes += s->plan_got;
    if (rc != 0) {
        /* rc == -1 here is always a flash failure, never a format2_err_t: FORMAT2_ESHORT is the only error that negates to -1, and it only fires before any step runs, which the length check above already ruled out. */
        if (rc == -1) {
            ack(s, NAK2_FLASH_FAIL);
            return fail(s, "format2: apply: %s", s->f->err.msg);
        }
        ack(s, (uint8_t)(-rc));
        return fail(s, "format2: plan walk refused: %s", format2_err_str((format2_err_t)(-rc)));
    }

    /* Stages the outcome into meta_t for lib/bootloader.c's existing PLAN_STAGING->TRIAL transition; op[]/src_slot[]/src_index[]/target_hash[] stay zeroed since format2_plan_walk() above already applied everything live, so the boot-time apply loop has nothing left to do. */
    double tp0 = stats_tick(s->stats);
    meta_load(s->f, &s->np);
    meta_reset_plan(&s->np);
    s->np.plan_state     = PLAN_STAGING;
    s->np.pending_slot   = (uint8_t)s->pending_slot;
    s->np.target_version = chunky_descriptor_body_version(body);
    s->np.image_len       = chunky_descriptor_body_image_len(body);
    s->np.step_count      = (uint16_t)step_count;
    if (meta_store(s->f, &s->np) < 0) {
        ack(s, NAK2_FLASH_FAIL);
        return fail(s, "persisting the plan: %s", s->f->err.msg);
    }
    s->stats->t_persist = stats_tick(s->stats) - tp0;

    log_fmt(s->log, s->log_ctx, "device: plan v%u, %u steps, applied into slot %c",
           s->np.target_version, step_count, 'A' + s->pending_slot);
    ack(s, 0);

    s->state = S_COMMIT;
    return DEV_PENDING;
}

static int on_offer(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    if (t == MSG2_NO_UPDATE) {
        log_fmt(s->log, s->log_ctx, "device: no update offered");
        s->state = S_DONE;
        return DEV_NO_UPDATE;
    }
    if (t != MSG2_PLAN) return fail(s, "protocol: expected PLAN or NO_UPDATE, got %d", t);
    return on_plan_frag(s, pay, n);
}

static int on_plan(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    if (t < 0) return fail(s, "link closed before the plan was complete");
    if (t == MSG2_PLAN) return on_plan_frag(s, pay, n);
    if (t != MSG2_PLAN_END) return fail(s, "protocol: expected PLAN or PLAN_END, got %d", t);
    return on_plan_end(s);
}

static int on_commit(device_session_t *s, int t)
{
    if (t == MSG2_COMMIT) {
        if (journal_set(s->f, JRN_ARM_INDEX, 0x0000) < 0)   /* arm: plan is committed */
            return fail(s, "arming the plan: %s", s->f->err.msg);
        log_fmt(s->log, s->log_ctx, "device: plan armed; reboot to trial-boot");
    } else {
        log_fmt(s->log, s->log_ctx,
                "device: no COMMIT, plan left unarmed; its writes are already in flash, "
                "so the boot discards the plan and the device needs another update");
    }
    s->state = S_DONE;
    return DEV_STAGED;
}

int device_session_feed(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    switch (s->state) {
    case S_OFFER:  return on_offer(s, t, pay, n);
    case S_PLAN:   return on_plan(s, t, pay, n);
    case S_COMMIT: return on_commit(s, t);
    default:        return fail(s, "session already over");
    }
}

int device_confirm(flash_t *f, log_fn log, void *log_ctx)
{
    meta_t m;
    meta_load(f, &m);
    if (m.plan_state != PLAN_TRIAL)
        return err_set(&f->err, "confirm: no trial boot in progress (plan_state=%d)",
                       m.plan_state);

    log_fmt(log, log_ctx, "device: confirming slot %c (trial boot succeeded)",
           'A' + m.pending_slot);

    /* SPEC.md 12.4: the floor only rises, on confirm, read back from flash, never trusted from the (long-gone) session. Floor 0 is safe here: this image already passed the real floor check at STAGING->TRIAL, which can only be <= today's floor. */
    uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
    chunky_descriptor_body_view_t body;
    if (format2_image_verify_flash(f, m.pending_slot, 0u, desc_raw, &body) == FORMAT2_OK) {
        uint32_t sc = chunky_descriptor_body_security_counter(body);
        if (sc > m.security_floor) m.security_floor = sc;
    }

    m.active_slot    = m.pending_slot;
    m.pending_slot   = SLOT_NONE;
    m.plan_state     = PLAN_COMMITTED;
    m.attempts_left  = 0;
    m.last_result    = RES_OK;
    return meta_store(f, &m);
}
