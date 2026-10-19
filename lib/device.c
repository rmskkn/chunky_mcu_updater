#include "device.h"
#include "image.h"
#include "compress.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static double tick(const device_stats_t *st)
{
    return st->clock_ms ? st->clock_ms() : 0.0;
}

/* Session states: which message the session expects next. */
enum { S_CHALLENGE, S_OFFER, S_OFFER_FULL, S_PLAN, S_COMMIT, S_DONE };

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

static void ack(device_session_t *s, uint8_t type, sector_idx_t dst, uint8_t code)
{
    msg_step_ack_t a = { dst, code };
    reply(s, type, &a, sizeof a);
}

uint32_t device_app_version(flash_t *f, int slot)
{
    image_desc_t d;
    image_desc_read(f, slot, &d);
    return d.magic == IMG_MAGIC ? d.version : 0;
}

int device_session_start(device_session_t *s, flash_t *f, const uint8_t dev_key[32],
                         device_send_fn send_fn, void *send_ctx,
                         log_fn log, void *log_ctx, device_stats_t *stats)
{
    memset(s, 0, sizeof *s);
    s->f = f;
    s->dev_key = dev_key;
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

    msg_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, s->device_id, 16);
    h.sector_size     = SECTOR_SIZE;
    h.sectors         = SECTORS_SLOT;
    h.last_result     = (uint8_t)m.last_result;
    h.sector_hash_len = SECTOR_HASH_LEN;
    h.active_slot     = (slot_id_t)s->active_slot;
    image_desc_t d;
    image_desc_read(f, s->active_slot, &d);
    h.running_version = d.magic == IMG_MAGIC ? d.version : 0;
    if (d.magic == IMG_MAGIC) image_hash_flash(f, s->active_slot, h.running_sha256);
    reply(s, MSG_HELLO, &h, sizeof h);

    s->state = S_CHALLENGE;
    return DEV_PENDING;
}

static int on_challenge(device_session_t *s, const uint8_t *pay, size_t n)
{
    msg_challenge_t ch;
    if (n < sizeof ch) return fail(s, "short CHALLENGE (%zu B)", n);
    memcpy(&ch, pay, sizeof ch);

    double t0 = tick(s->stats);
    manifest_build(s->f, &s->man);
    s->stats->t_manifest = tick(s->stats) - t0;
    s->stats->sha_bytes += (size_t)MANIFEST_ENTRIES * SECTOR_SIZE;

    t0 = tick(s->stats);
    manifest_digest(&s->man, s->auth.manifest_digest);
    manifest_mac(s->dev_key, s->device_id, ch.nonce, s->auth.manifest_digest, s->auth.mac);
    s->stats->t_auth = tick(s->stats) - t0;
    s->stats->sha_bytes += MANIFEST_BODY_LEN;
    s->stats->ecdsa_ms += ECDSA_SIGN_MS;        /* the real signature, not this HMAC */
    reply(s, MSG_MANIFEST_DIGEST, &s->auth, sizeof s->auth);

    s->state = S_OFFER;
    return DEV_PENDING;
}

static void send_manifest(device_session_t *s)
{
    const uint8_t *body = (const uint8_t *)&s->man;
    for (size_t off = 0; off < MANIFEST_BODY_LEN; off += 256) {
        uint8_t buf[4 + 256];
        uint16_t len = (uint16_t)(MANIFEST_BODY_LEN - off < 256 ? MANIFEST_BODY_LEN - off : 256);
        uint16_t o16 = (uint16_t)off;
        memcpy(buf, &o16, 2);
        memcpy(buf + 2, &len, 2);
        memcpy(buf + 4, body + off, len);
        reply(s, MSG_MANIFEST, buf, 4 + len);
    }
    reply(s, MSG_MANIFEST_END, &s->auth, sizeof s->auth);
    log_fmt(s->log, s->log_ctx, "device: full manifest sent (%zu B of body)", MANIFEST_BODY_LEN);
}

/* The server's answer to the manifest: nothing, a rejection, or a plan. */
static int on_offer(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    if (t == MSG_NO_UPDATE) {
        log_fmt(s->log, s->log_ctx, "device: no update offered");
        s->state = S_DONE;
        return DEV_NO_UPDATE;
    }
    if (t == MSG_MANIFEST_REJECT) {
        log_fmt(s->log, s->log_ctx, "device: manifest rejected, reason %u", n ? pay[0] : 0);
        s->state = S_DONE;
        return DEV_NO_UPDATE;
    }
    if (t != MSG_PLAN_BEGIN) return fail(s, "protocol: expected PLAN_BEGIN, got %d", t);

    msg_plan_begin_t pb;
    if (n < sizeof pb) return fail(s, "short PLAN_BEGIN (%zu B)", n);
    memcpy(&pb, pay, sizeof pb);

    /* pending_slot is always "the slot not currently active," computed from
     * our own stored active_slot -- never from pb.dest_slot, which is only
     * cross-checked below (docs/ab_migration.md: "never from anything the
     * wire sends"). */
    s->pending_slot = OTHER_SLOT(s->active_slot);
    if (pb.dest_slot != s->pending_slot)
        return fail(s, "server's plan targets slot %d, this device's inactive slot is %d",
                   pb.dest_slot, s->pending_slot);
    log_fmt(s->log, s->log_ctx, "device: plan v%u, %u steps, into slot %c",
           pb.version, pb.step_count, 'A' + s->pending_slot);

    /* Reload rather than meta_init(): a fresh plan is per-cycle scratch, but
     * active_slot (and, once a plan exists, the attempt log) must survive
     * every cycle including this one if it never lands -- meta_reset_plan()
     * clears only the scratch fields. */
    meta_load(s->f, &s->np);
    meta_reset_plan(&s->np);
    s->np.plan_state     = PLAN_STAGING;
    s->np.pending_slot   = (uint8_t)s->pending_slot;
    s->np.target_version = pb.version;
    s->np.step_count     = pb.step_count;
    memcpy(s->np.target_sha256, pb.image_sha256, SHA256_LEN);
    memcpy(s->np.target_mac, pb.image_mac, SHA256_LEN);
    memset(s->done, 0, sizeof s->done);
    s->pending_copies = 0;
    s->t0 = tick(s->stats);

    s->state = S_PLAN;
    return DEV_PENDING;
}

/* Writes the decoded sector to dst once its hash matches the sender's. */
static void stage_sector(device_session_t *s, sector_idx_t dst,
                         const uint8_t hash[SECTOR_HASH_LEN])
{
    uint8_t got[SECTOR_HASH_LEN];
    sector_hash(s->sect, got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        ack(s, MSG_STEP_NAK, dst, NAK_HASH_MISMATCH);
        log_fmt(s->log, s->log_ctx, "device: sector %u hash mismatch", dst);
        return;
    }

    /* The sector may already hold this exact content - a stale manifest
     * cache or a re-sent step, say. Check before erasing: an erase/program
     * cycle is real wear and the slow part of this step, a hash is neither. */
    flash_sector_hash(s->f, s->pending_slot, dst, got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        if (flash_write_app_sector(s->f, s->pending_slot, s->active_slot, dst, s->sect) < 0) {
            ack(s, MSG_STEP_NAK, dst, NAK_FLASH_FAIL);
            log_fmt(s->log, s->log_ctx, "device: sector %u: %s", dst, s->f->err.msg);
            return;
        }
        flash_sector_hash(s->f, s->pending_slot, dst, got);
    }

    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        ack(s, MSG_STEP_NAK, dst, NAK_FLASH_FAIL);
        return;
    }
    s->np.op[dst] = OP_WRITE;
    memcpy(s->np.target_hash[dst], hash, SECTOR_HASH_LEN);
    s->done[dst] = 1;
    s->stats->sha_bytes += 2 * SECTOR_SIZE;   /* hash of the buffer and of the sector in flash */
    ack(s, MSG_STEP_ACK, dst, 0);
}

static int on_plan_end(device_session_t *s)
{
    s->stats->t_stage = tick(s->stats) - s->t0;

    /* Persist the plan once, then mark the sectors already in flash.
     * meta_store() erases the target bank, so it must come first. */
    double t0 = tick(s->stats);
    if (meta_store(s->f, &s->np) < 0)
        return fail(s, "persisting the plan: %s", s->f->err.msg);
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (s->done[i] && journal_set(s->f, i, JRN_VERIFIED) < 0)
            return fail(s, "journal: %s", s->f->err.msg);
    s->stats->t_persist = tick(s->stats) - t0;

    msg_plan_ready_t pr = { (uint16_t)s->pending_copies };
    reply(s, MSG_PLAN_READY, &pr, sizeof pr);

    s->state = S_COMMIT;
    return DEV_PENDING;
}

static int on_step(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    /* In place, every WRITE so far is already in flash and nothing records
     * it: the plan is persisted only at PLAN_END. The next request's
     * manifest describes those sectors as they are now, so the server
     * re-diffs from them; until then the partition holds a mix. */
    if (t < 0) return fail(s, "link closed mid-plan; sectors written so far stay as written");
    if (t == MSG_PLAN_END) return on_plan_end(s);

    if (t == MSG_STEP_COPY) {
        msg_step_copy_t c;
        if (n < sizeof c) return fail(s, "short STEP_COPY (%zu B)", n);
        memcpy(&c, pay, sizeof c);
        if (c.dst >= SECTORS_SLOT || c.src_index >= SECTORS_SLOT ||
            (c.src_slot != SLOT_A && c.src_slot != SLOT_B))
            return fail(s, "bad sector index dst=%u src_slot=%u src=%u",
                       c.dst, c.src_slot, c.src_index);
        /* Recorded only. The bootloader does flash-to-flash copies
         * (DESIGN.md section 7 step 2). */
        s->np.op[c.dst]        = OP_COPY;
        s->np.src_slot[c.dst]  = c.src_slot;
        s->np.src_index[c.dst] = c.src_index;
        memcpy(s->np.target_hash[c.dst], c.hash, SECTOR_HASH_LEN);
        s->pending_copies++;
        ack(s, MSG_STEP_ACK, c.dst, 0);
    } else if (t == MSG_STEP_WRITE) {
        sector_idx_t dst;
        uint16_t off, len;
        if (n < WRITE_HDR) return fail(s, "short STEP_WRITE (%zu B)", n);
        memcpy(&dst, pay + 0, 2);
        memcpy(&off, pay + 2, 2);
        memcpy(&len, pay + 4, 2);
        if (dst >= SECTORS_SLOT || (size_t)off + len > SECTOR_SIZE || len > n - WRITE_HDR)
            return fail(s, "bad write fragment");
        memcpy(s->enc + off, pay + WRITE_HDR, len);
    } else if (t == MSG_STEP_WRITE_END) {
        msg_step_write_end_t we;
        if (n < sizeof we) return fail(s, "short STEP_WRITE_END (%zu B)", n);
        memcpy(&we, pay, sizeof we);
        if (we.dst >= SECTORS_SLOT || we.enc_len > SECTOR_SIZE ||
            (we.compress == COMPRESS_RAW && we.enc_len != SECTOR_SIZE))
            return fail(s, "bad write end");

        /* The dictionary is dst's current flash content in the pending
         * slot, which is what our manifest told the server it holds: dst is
         * written once per plan, and only after this. */
        flash_read(s->f, slot_base(s->pending_slot) + (size_t)we.dst * SECTOR_SIZE,
                  s->dict, SECTOR_SIZE);
        if (decompress_sector(we.compress, s->enc, we.enc_len, s->dict, s->sect) < 0) {
            ack(s, MSG_STEP_NAK, we.dst, NAK_DECOMPRESS);
            log_fmt(s->log, s->log_ctx, "device: sector %u does not decompress", we.dst);
            return DEV_PENDING;
        }
        stage_sector(s, we.dst, we.hash);
    } else {
        return fail(s, "protocol: unexpected message %d during plan", t);
    }
    return DEV_PENDING;
}

static int on_commit(device_session_t *s, int t)
{
    if (t == MSG_COMMIT) {
        if (journal_set(s->f, JRN_ARM_INDEX, 0x0000) < 0)   /* arm: plan is committed */
            return fail(s, "arming the plan: %s", s->f->err.msg);
        log_fmt(s->log, s->log_ctx,
                "device: plan armed, %d copies left for the bootloader; reboot to apply",
                s->pending_copies);
    } else {
        /* Nothing to unroll in place: the WRITEs are already in flash. */
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
    case S_CHALLENGE:
        if (t < 0) return fail(s, "no CHALLENGE");
        if (t == MSG_MANIFEST_REJECT) {
            /* HELLO refused: geometry or hash width differs from the server's. */
            log_fmt(s->log, s->log_ctx, "device: session refused, reason %u", n ? pay[0] : 0);
            s->state = S_DONE;
            return DEV_NO_UPDATE;
        }
        if (t != MSG_CHALLENGE) return fail(s, "protocol: expected CHALLENGE, got %d", t);
        return on_challenge(s, pay, n);
    case S_OFFER:
        if (t == MSG_MANIFEST_NEEDED) {
            send_manifest(s);
            s->state = S_OFFER_FULL;
            return DEV_PENDING;
        }
        log_fmt(s->log, s->log_ctx, "device: manifest digest accepted, body elided");
        return on_offer(s, t, pay, n);
    case S_OFFER_FULL:
        return on_offer(s, t, pay, n);
    case S_PLAN:
        return on_step(s, t, pay, n);
    case S_COMMIT:
        return on_commit(s, t);
    default:
        return fail(s, "session already over");
    }
}

/* The bootloader could not record what it found: the meta banks keep their
 * last good state, and the next boot sees the same plan again. */
static int boot_flash_fail(flash_t *f, log_fn log, void *log_ctx)
{
    log_fmt(log, log_ctx, "boot: cannot record the result: %s", f->err.msg);
    return -1;
}

/* Unroll (DESIGN.md section 8): abandon a plan that never booted -- never
 * armed, a copy source gone stale, or the staged image failing verify.
 * Erasing just sector 0 of the pending slot is enough: the slot header
 * disappears, so it is unmistakably invalid, and the apply invariant means
 * there is nothing else to undo. active_slot is untouched; it was never
 * going to move until a confirm that now never happens. */
static int unroll_plan(flash_t *f, meta_t *m, log_fn log, void *log_ctx, const char *why)
{
    int pending = m->pending_slot;
    log_fmt(log, log_ctx, "boot: unrolling the plan into slot %c (%s)", 'A' + pending, why);
    size_t off = slot_base(pending);
    for (int u = 0; u < SECTOR_SIZE / ERASE_SIZE; u++)
        if (flash_erase_unit(f, (int)(off / ERASE_SIZE) + u) < 0) return -1;
    m->plan_state    = PLAN_NONE;
    m->pending_slot  = SLOT_NONE;
    m->attempts_left = 0;
    m->last_result   = RES_VERIFY_FAILED;
    return meta_store(f, m);
}

/* Revert (DESIGN.md section 8): a committed-but-broken image, after
 * MAX_TRIES failed trial boots. active_slot is unchanged -- it was never
 * moved, so the image that was running before the update is still there.
 * The pending slot's descriptor is marked REVOKED so it is never retried
 * as-is; it is now simply the inactive slot again, available as the
 * destination of the next plan. */
static int revert_plan(flash_t *f, meta_t *m, log_fn log, void *log_ctx)
{
    int pending = m->pending_slot;
    log_fmt(log, log_ctx, "boot: reverting after %d failed trial boots; "
           "slot %c revoked, staying on slot %c",
           MAX_TRIES, 'A' + pending, 'A' + m->active_slot);
    if (image_revoke_flash(f, pending) < 0) return -1;
    m->plan_state    = PLAN_NONE;
    m->pending_slot  = SLOT_NONE;
    m->attempts_left = 0;
    m->last_result   = RES_TRIAL_FAILED;
    return meta_store(f, m);
}

/* Applies whatever the session staged into the pending slot, trial-boots
 * it, and otherwise decides whether the active slot is bootable (DESIGN.md
 * sections 7-8). See device.h for the calling contract: one call is one
 * simulated boot/reset cycle. */
int device_boot(flash_t *f, const uint8_t root_key[32],
                log_fn log, void *log_ctx, device_stats_t *stats)
{
    device_stats_t dummy = { 0 };
    if (!stats) stats = &dummy;
    stats->t_apply = stats->t_verify = 0;

    meta_t m;
    const char *why = NULL;

    meta_load(f, &m);

    if (m.plan_state == PLAN_STAGING && journal_get(f, JRN_ARM_INDEX) != 0x0000) {
        if (unroll_plan(f, &m, log, log_ctx, "plan was never committed") < 0)
            return boot_flash_fail(f, log, log_ctx);
        goto final_boot;   /* unroll leaves nothing pending; boot active_slot */
    } else if (m.plan_state == PLAN_STAGING) {
        int pending = m.pending_slot;
        log_fmt(log, log_ctx, "boot: applying plan v%u into slot %c",
               m.target_version, 'A' + pending);
        double t0 = tick(stats);
        int bad = 0;
        for (int i = 0; i < SECTORS_SLOT && !bad; i++) {
            if (m.op[i] == OP_NONE || journal_get(f, i) == JRN_DONE) continue;

            uint8_t got[SECTOR_HASH_LEN];
            if (m.op[i] == OP_COPY) {
                /* src_slot/src_index came off the wire a session ago and
                 * live in flash across the reboot; the bounds check at send
                 * time (device_session_feed) does not protect this read,
                 * which runs on every boot regardless of how the stored
                 * value got there. Treat an out-of-range index, or a slot
                 * id that is neither A nor B, as a bad plan, same as a copy
                 * source that no longer matches -- this path must never
                 * read outside the two slots. */
                if (m.src_index[i] >= SECTORS_SLOT ||
                    (m.src_slot[i] != SLOT_A && m.src_slot[i] != SLOT_B)) {
                    if (unroll_plan(f, &m, log, log_ctx, "copy source out of range") < 0)
                        return boot_flash_fail(f, log, log_ctx);
                    bad = 1;
                    break;
                }
                /* Flash to flash, through RAM; source is always safe to
                 * read (either the untouched active slot, or a sector of
                 * the pending slot the plan itself already verified as
                 * correct and left alone -- lib/plan.c's rule 2). */
                uint8_t tmp[SECTOR_SIZE];
                flash_read(f, slot_base(m.src_slot[i]) + (size_t)m.src_index[i] * SECTOR_SIZE,
                           tmp, SECTOR_SIZE);
                sector_hash(tmp, got);
                if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                    if (unroll_plan(f, &m, log, log_ctx, "copy source no longer matches") < 0)
                        return boot_flash_fail(f, log, log_ctx);
                    bad = 1;
                    break;
                }
                if (flash_write_app_sector(f, pending, m.active_slot, (sector_idx_t)i, tmp) < 0) {
                    log_fmt(log, log_ctx, "boot: copy into sector %d: %s", i, f->err.msg);
                    if (unroll_plan(f, &m, log, log_ctx, "flash failure during copy") < 0)
                        return boot_flash_fail(f, log, log_ctx);
                    bad = 1;
                    break;
                }
            }
            flash_sector_hash(f, pending, i, got);
            if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                if (unroll_plan(f, &m, log, log_ctx, "sector does not match the plan") < 0)
                    return boot_flash_fail(f, log, log_ctx);
                bad = 1;
                break;
            }
            if (journal_set(f, i, JRN_DONE) < 0) {
                log_fmt(log, log_ctx, "boot: journal for sector %d: %s", i, f->err.msg);
                if (unroll_plan(f, &m, log, log_ctx, "flash failure recording progress") < 0)
                    return boot_flash_fail(f, log, log_ctx);
                bad = 1;
                break;
            }
            stats->sha_bytes += 2 * SECTOR_SIZE;
        }
        stats->t_apply = tick(stats) - t0;
        /* unroll_plan() already recorded and stored the result; boot
         * active_slot, which this failed apply never touched. */
        if (bad) goto final_boot;

        t0 = tick(stats);
        uint8_t hsh[SHA256_LEN];
        image_hash_flash(f, pending, hsh);
        stats->sha_bytes += SLOT_SIZE - DESC_LEN;
        stats->ecdsa_ms += ECDSA_VERIFY_MS;
        if (memcmp(hsh, m.target_sha256, SHA256_LEN) ||
            image_verify_flash(f, pending, root_key, hsh, &why) < 0) {
            stats->t_verify = tick(stats) - t0;
            if (unroll_plan(f, &m, log, log_ctx,
                            why ? why : "image_sha256 mismatch") < 0)
                return boot_flash_fail(f, log, log_ctx);
            goto final_boot;
        }
        stats->t_verify = tick(stats) - t0;

        /* DESIGN.md section 7 steps 4-5: the pending image verified, so it
         * becomes a trial -- MAX_TRIES tokens, then immediately consume one
         * for this same boot/reset cycle (see device.h's calling contract:
         * the STAGING->TRIAL transition and the first trial attempt happen
         * in one device_boot() call, exactly like a real bootloader would
         * do between one apply pass and the jump that follows it). */
        m.plan_state    = PLAN_TRIAL;
        m.attempts_left = MAX_TRIES;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
    }

    if (m.plan_state == PLAN_TRIAL) {
        if (m.attempts_left == 0) {
            if (revert_plan(f, &m, log, log_ctx) < 0) return boot_flash_fail(f, log, log_ctx);
            goto final_boot;   /* revert leaves nothing pending; boot active_slot */
        }
        m.attempts_left--;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
        log_fmt(log, log_ctx,
               "boot: booting version %u (trial boot, slot %c, attempt %d/%d; "
               "call device_confirm() or this attempt is wasted)",
               device_app_version(f, m.pending_slot), 'A' + m.pending_slot,
               MAX_TRIES - m.attempts_left, MAX_TRIES);
        /* Unlike every other path here, this call ends with the *pending*
         * slot running on trial, not active_slot -- there is nothing to
         * fall through to. */
        return 0;
    }

final_boot:
    /* plan_state is NONE or COMMITTED: an ordinary boot of active_slot.
     * The signature is not re-checked here (DESIGN.md section 10.1) -- only
     * image_sha256, cheap enough to pay every boot, which is what turns bit
     * rot in a long-deployed image into a clean fallback instead of an
     * undefined crash. */
    uint8_t hsh[SHA256_LEN];
    image_hash_flash(f, m.active_slot, hsh);
    stats->sha_bytes += SLOT_SIZE - DESC_LEN;
    if (image_verify_flash(f, m.active_slot, root_key, hsh, &why) < 0) {
        log_fmt(log, log_ctx, "boot: active slot %c does not verify (%s)",
               'A' + m.active_slot, why);
        int other = OTHER_SLOT(m.active_slot);
        uint8_t hsh2[SHA256_LEN];
        const char *why2 = NULL;
        image_hash_flash(f, other, hsh2);
        if (image_verify_flash(f, other, root_key, hsh2, &why2) == 0) {
            log_fmt(log, log_ctx, "boot: falling back to slot %c", 'A' + other);
            m.active_slot = (uint8_t)other;
            m.last_result = RES_VERIFY_FAILED;
            if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
            log_fmt(log, log_ctx, "boot: booting version %u", device_app_version(f, m.active_slot));
        } else {
            /* Both slots unusable: this is as far as the device gets. A
             * real port enters the recovery loop here (DESIGN.md section
             * 3); the PoC just reports it and asks for another release. */
            log_fmt(log, log_ctx, "boot: no bootable image in either slot (%s); "
                   "requesting an update", why2 ? why2 : why);
            if (m.last_result != RES_VERIFY_FAILED) {
                m.last_result = RES_VERIFY_FAILED;
                if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
            }
        }
    } else {
        log_fmt(log, log_ctx, "boot: booting version %u", device_app_version(f, m.active_slot));
    }
    return 0;
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
    m.active_slot    = m.pending_slot;
    m.pending_slot   = SLOT_NONE;
    m.plan_state     = PLAN_COMMITTED;
    m.attempts_left  = 0;
    m.last_result    = RES_OK;
    return meta_store(f, &m);
}
