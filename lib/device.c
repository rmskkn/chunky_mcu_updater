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

uint32_t device_app_version(flash_t *f)
{
    image_desc_t d;
    image_desc_read(f, &d);
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
    s->generation = m.generation;

    msg_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, s->device_id, 16);
    h.sector_size     = SECTOR_SIZE;
    h.sectors         = SECTORS_SLOT;
    h.last_result     = (uint8_t)m.last_result;
    h.sector_hash_len = SECTOR_HASH_LEN;
    image_desc_t d;
    image_desc_read(f, &d);
    h.running_version = d.magic == IMG_MAGIC ? d.version : 0;
    if (d.magic == IMG_MAGIC) image_hash_flash(f, h.running_sha256);
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
    log_fmt(s->log, s->log_ctx, "device: plan v%u, %u steps", pb.version, pb.step_count);

    meta_init(&s->np);
    s->np.generation     = s->generation;
    s->np.plan_state     = PLAN_STAGING;
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
    flash_sector_hash(s->f, dst, got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        if (flash_write_app_sector(s->f, dst, s->sect) < 0) {
            ack(s, MSG_STEP_NAK, dst, NAK_FLASH_FAIL);
            log_fmt(s->log, s->log_ctx, "device: sector %u: %s", dst, s->f->err.msg);
            return;
        }
        flash_sector_hash(s->f, dst, got);
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
        if (c.dst >= SECTORS_SLOT || c.src_index >= SECTORS_SLOT)
            return fail(s, "bad sector index dst=%u src=%u", c.dst, c.src_index);
        /* Recorded only. The bootloader does flash-to-flash copies
         * (DESIGN.md section 7 step 2). */
        s->np.op[c.dst]        = OP_COPY;
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

        /* The dictionary is dst's current flash content, which is what our
         * manifest told the server it holds: dst is written once per plan,
         * and only after this. */
        flash_read(s->f, APP_BASE + (size_t)we.dst * SECTOR_SIZE, s->dict, SECTOR_SIZE);
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

/* Applies whatever the session staged, then decides whether the partition is
 * bootable. With one partition there is nothing to fall back to: a failure is
 * reported and the next request is the recovery path. */
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
        log_fmt(log, log_ctx, "boot: plan was never committed, discarding it "
                "(the sectors it wrote stay as written)");
        m.plan_state  = PLAN_NONE;
        m.last_result = RES_VERIFY_FAILED;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
    } else if (m.plan_state == PLAN_STAGING) {
        log_fmt(log, log_ctx, "boot: applying plan v%u", m.target_version);
        double t0 = tick(stats);
        for (int i = 0; i < SECTORS_SLOT; i++) {
            if (m.op[i] == OP_NONE || journal_get(f, i) == JRN_DONE) continue;

            uint8_t got[SECTOR_HASH_LEN];
            if (m.op[i] == OP_COPY) {
                /* src_index came off the wire a session ago and lives in
                 * flash across the reboot; a bounds check at send time
                 * (device_session_feed) does not protect this read, which
                 * runs on every boot regardless of how the stored value got
                 * there. Treat an out-of-range index as a bad plan, same as
                 * a copy source that no longer matches -- this path must
                 * never read outside the partition. */
                if (m.src_index[i] >= SECTORS_SLOT) {
                    log_fmt(log, log_ctx, "boot: copy source for sector %d is out of range", i);
                    m.plan_state = PLAN_BAD;
                    break;
                }
                /* Flash to flash inside the partition, through RAM. */
                uint8_t tmp[SECTOR_SIZE];
                flash_read(f, APP_BASE + (size_t)m.src_index[i] * SECTOR_SIZE,
                           tmp, SECTOR_SIZE);
                sector_hash(tmp, got);
                if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                    log_fmt(log, log_ctx, "boot: copy source for sector %d no longer matches", i);
                    m.plan_state = PLAN_BAD;
                    break;
                }
                if (flash_write_app_sector(f, (sector_idx_t)i, tmp) < 0) {
                    log_fmt(log, log_ctx, "boot: copy into sector %d: %s", i, f->err.msg);
                    m.plan_state  = PLAN_BAD;
                    m.last_result = RES_FLASH_FAIL;
                    break;
                }
            }
            flash_sector_hash(f, i, got);
            if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                log_fmt(log, log_ctx, "boot: sector %d does not match the plan", i);
                m.plan_state = PLAN_BAD;
                break;
            }
            if (journal_set(f, i, JRN_DONE) < 0) {
                log_fmt(log, log_ctx, "boot: journal for sector %d: %s", i, f->err.msg);
                m.plan_state  = PLAN_BAD;
                m.last_result = RES_FLASH_FAIL;
                break;
            }
            stats->sha_bytes += 2 * SECTOR_SIZE;
        }
        stats->t_apply = tick(stats) - t0;

        if (m.plan_state != PLAN_BAD) {
            t0 = tick(stats);
            uint8_t hsh[SHA256_LEN];
            image_hash_flash(f, hsh);
            stats->sha_bytes += SLOT_SIZE - DESC_LEN;
            stats->ecdsa_ms += ECDSA_VERIFY_MS;
            if (memcmp(hsh, m.target_sha256, SHA256_LEN) ||
                image_verify_flash(f, root_key, hsh, &why) < 0) {
                log_fmt(log, log_ctx, "boot: the staged image does not verify (%s)",
                       why ? why : "image_sha256 mismatch");
                m.plan_state = PLAN_BAD;
            } else {
                m.plan_state  = PLAN_COMMITTED;
                m.last_result = RES_OK;
            }
            stats->t_verify = tick(stats) - t0;
        }
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
    }

    uint8_t hsh[SHA256_LEN];
    image_hash_flash(f, hsh);
    stats->sha_bytes += SLOT_SIZE - DESC_LEN;
    if (image_verify_flash(f, root_key, hsh, &why) < 0) {
        /* No second partition, so this is as far as the device gets: it
         * keeps asking for a release until one verifies. */
        log_fmt(log, log_ctx, "boot: no bootable image (%s); requesting an update", why);
        if (m.plan_state != PLAN_BAD) {
            m.plan_state  = PLAN_BAD;
            m.last_result = RES_VERIFY_FAILED;
            if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
        }
    } else {
        log_fmt(log, log_ctx, "boot: booting version %u", device_app_version(f));
    }
    return 0;
}
