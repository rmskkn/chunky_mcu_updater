#include "device.h"
#include "image.h"
#include "compress.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Session states: which message the session expects next. */
enum { S_CHALLENGE, S_OFFER, S_OFFER_FULL, S_TABLE, S_PLAN, S_COMMIT, S_DONE };

_Static_assert(SECTORS_SLOT * SECTOR_HASH_LEN <= 0xFFFF, "PLAN_TABLE offsets are u16");

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

int device_session_start(device_session_t *s, flash_t *f, const uint8_t dev_key[32],
                         const uint8_t root_key[32], device_send_fn send_fn, void *send_ctx,
                         log_fn log, void *log_ctx, device_stats_t *stats)
{
    memset(s, 0, sizeof *s);
    s->f = f;
    s->dev_key = dev_key;
    s->root_key = root_key;
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
    if (d.magic == IMG_MAGIC) image_hash_flash(f, s->active_slot, d.image_len, h.running_sha256);
    reply(s, MSG_HELLO, &h, sizeof h);

    s->state = S_CHALLENGE;
    return DEV_PENDING;
}

static int on_challenge(device_session_t *s, const uint8_t *pay, size_t n)
{
    msg_challenge_t ch;
    if (n < sizeof ch) return fail(s, "short CHALLENGE (%zu B)", n);
    memcpy(&ch, pay, sizeof ch);

    double t0 = stats_tick(s->stats);
    manifest_build(s->f, &s->man);
    s->stats->t_manifest = stats_tick(s->stats) - t0;
    s->stats->sha_bytes += (size_t)MANIFEST_ENTRIES * SECTOR_SIZE;

    t0 = stats_tick(s->stats);
    manifest_digest(&s->man, s->auth.manifest_digest);
    manifest_mac(s->dev_key, s->device_id, ch.nonce, s->auth.manifest_digest, s->auth.mac);
    s->stats->t_auth = stats_tick(s->stats) - t0;
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
    if (!image_len_ok(pb.image_len))
        return fail(s, "plan image_len %u is not a sector multiple in [%d, %d]",
                   pb.image_len, CODE_OFF, SLOT_SIZE);
    if (pb.step_count > pb.image_len / SECTOR_SIZE)
        return fail(s, "plan has %u steps for a %u-sector image",
                   pb.step_count, pb.image_len / SECTOR_SIZE);
    s->n_sectors = (int)(pb.image_len / SECTOR_SIZE);

    /* The release signature, before the first step: the descriptor's other
     * fields are fixed by the format, so the device rebuilds it whole, and
     * PLAN_END compares it with what lands in flash. */
    image_desc_t *d = &s->desc;
    uint8_t mac[SHA256_LEN];
    memset(d, 0, sizeof *d);
    d->magic     = IMG_MAGIC;
    d->version   = pb.version;
    d->image_len = pb.image_len;
    d->flags     = pb.flags;
    d->revoked   = IMG_NOT_REVOKED;
    memcpy(d->sha256, pb.image_sha256, SHA256_LEN);
    memcpy(d->table_root, pb.table_root, SHA256_LEN);
    memcpy(d->mac, pb.image_mac, SHA256_LEN);
    image_desc_mac(d, s->root_key, mac);
    s->stats->ecdsa_ms += ECDSA_VERIFY_MS;
    if (memcmp(mac, pb.image_mac, SHA256_LEN))
        return fail(s, "plan's release signature does not verify");
    if (pb.version < image_version_flash(s->f, s->active_slot) && !(pb.flags & 1))
        return fail(s, "plan v%u would downgrade the running v%u",
                   pb.version, image_version_flash(s->f, s->active_slot));
    log_fmt(s->log, s->log_ctx, "device: plan v%u, %u steps over %d sectors, into slot %c",
           pb.version, pb.step_count, s->n_sectors, 'A' + s->pending_slot);

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
    s->np.image_len      = pb.image_len;
    memcpy(s->np.target_sha256, pb.image_sha256, SHA256_LEN);
    memcpy(s->np.target_mac, pb.image_mac, SHA256_LEN);
    memset(s->done, 0, sizeof s->done);
    s->pending_copies = 0;
    s->table_got = 0;
    s->desc_seen = 0;
    s->t0 = stats_tick(s->stats);

    s->state = S_TABLE;
    return DEV_PENDING;
}

/* The target's per-sector hashes, straight into the plan's dense
 * target_hash array, then authenticated as a whole against the signed
 * table_root. No step is accepted before that. */
static int on_table(device_session_t *s, int t, const uint8_t *pay, size_t n)
{
    size_t table_len = (size_t)s->n_sectors * SECTOR_HASH_LEN;

    if (t < 0) return fail(s, "link closed before the plan's table was complete");
    if (t == MSG_PLAN_TABLE) {
        uint16_t off, len;
        if (n < TABLE_HDR) return fail(s, "short PLAN_TABLE (%zu B)", n);
        memcpy(&off, pay, 2);
        memcpy(&len, pay + 2, 2);
        if (off != s->table_got || (size_t)off + len > table_len || len > n - TABLE_HDR)
            return fail(s, "bad PLAN_TABLE fragment");
        memcpy((uint8_t *)s->np.target_hash + off, pay + TABLE_HDR, len);
        s->table_got += len;
        return DEV_PENDING;
    }
    if (t != MSG_PLAN_TABLE_END)
        return fail(s, "protocol: expected the plan's table before any step, got %d", t);

    uint8_t root[SHA256_LEN];
    image_table_root((const uint8_t *)s->np.target_hash, (size_t)s->n_sectors, root);
    s->stats->sha_bytes += table_len;
    if (s->table_got != table_len || memcmp(root, s->desc.table_root, SHA256_LEN))
        return fail(s, "the plan's table does not match the signed table_root");
    s->state = S_PLAN;
    return DEV_PENDING;
}

/* Writes the decoded sector to dst once its hash matches the authenticated
 * table's entry. */
static void stage_sector(device_session_t *s, sector_idx_t dst)
{
    const uint8_t *hash = s->np.target_hash[dst];
    uint8_t got[SECTOR_HASH_LEN], raw[SECTOR_HASH_LEN];
    image_sector_hash(dst, s->sect, got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        ack(s, MSG_STEP_NAK, dst, NAK_HASH_MISMATCH);
        log_fmt(s->log, s->log_ctx, "device: sector %u hash mismatch", dst);
        return;
    }

    /* The sector may already hold this exact content - a stale manifest
     * cache or a re-sent step, say. Check before erasing: an erase/program
     * cycle is real wear and the slow part of this step, a hash is neither.
     * This compares every byte, which image_sector_hash() does not for the
     * descriptor sector. */
    sector_hash(s->sect, raw);
    flash_sector_hash(s->f, s->pending_slot, dst, got);
    if (memcmp(got, raw, SECTOR_HASH_LEN)) {
        if (flash_write_app_sector(s->f, s->pending_slot, s->active_slot, dst, s->sect) < 0) {
            ack(s, MSG_STEP_NAK, dst, NAK_FLASH_FAIL);
            log_fmt(s->log, s->log_ctx, "device: sector %u: %s", dst, s->f->err.msg);
            return;
        }
        flash_sector_hash(s->f, s->pending_slot, dst, got);
    }

    if (memcmp(got, raw, SECTOR_HASH_LEN)) {
        ack(s, MSG_STEP_NAK, dst, NAK_FLASH_FAIL);
        return;
    }
    s->np.op[dst] = OP_WRITE;
    s->done[dst] = 1;
    s->stats->sha_bytes += 2 * SECTOR_SIZE;   /* hash of the buffer and of the sector in flash */
    ack(s, MSG_STEP_ACK, dst, 0);
}

static int on_plan_end(device_session_t *s)
{
    s->stats->t_stage = stats_tick(s->stats) - s->t0;

    /* Whether the descriptor sector was written this session or the planner
     * found it already in place, what the pending slot now holds must be the
     * signed descriptor and a sector the table vouches for. */
    image_desc_t fd;
    uint8_t h[SECTOR_HASH_LEN];
    image_desc_read(s->f, s->pending_slot, &fd);
    image_sector_hash_flash(s->f, s->pending_slot, DESC_SECTOR, h);
    if (memcmp(&fd, &s->desc, sizeof fd))
        return fail(s, "the staged descriptor differs from the plan's signed one");
    if (memcmp(h, s->np.target_hash[DESC_SECTOR], SECTOR_HASH_LEN))
        return fail(s, "the staged descriptor sector does not match the plan's table");

    /* Persist the plan once, then mark the sectors already in flash.
     * meta_store() erases the target bank, so it must come first. */
    double t0 = stats_tick(s->stats);
    if (meta_store(s->f, &s->np) < 0)
        return fail(s, "persisting the plan: %s", s->f->err.msg);
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (s->done[i] && journal_set(s->f, i, JRN_VERIFIED) < 0)
            return fail(s, "journal: %s", s->f->err.msg);
    s->stats->t_persist = stats_tick(s->stats) - t0;

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

    /* The descriptor sector is the commit marker: the pending slot stays
     * without a valid header until it lands, so it is the last step. */
    if (s->desc_seen)
        return fail(s, "protocol: message %d after the descriptor sector", t);

    if (t == MSG_STEP_COPY) {
        msg_step_copy_t c;
        if (n < sizeof c) return fail(s, "short STEP_COPY (%zu B)", n);
        memcpy(&c, pay, sizeof c);
        if (c.dst >= s->n_sectors || c.dst == DESC_SECTOR || c.src_index >= SECTORS_SLOT ||
            (c.src_slot != SLOT_A && c.src_slot != SLOT_B))
            return fail(s, "bad sector index dst=%u src_slot=%u src=%u",
                       c.dst, c.src_slot, c.src_index);
        /* Recorded only. The bootloader does flash-to-flash copies
         * (DESIGN.md section 7 step 2). */
        s->np.op[c.dst]        = OP_COPY;
        s->np.src_slot[c.dst]  = c.src_slot;
        s->np.src_index[c.dst] = c.src_index;
        s->pending_copies++;
        ack(s, MSG_STEP_ACK, c.dst, 0);
    } else if (t == MSG_STEP_WRITE) {
        sector_idx_t dst;
        uint16_t off, len;
        if (n < WRITE_HDR) return fail(s, "short STEP_WRITE (%zu B)", n);
        memcpy(&dst, pay + 0, 2);
        memcpy(&off, pay + 2, 2);
        memcpy(&len, pay + 4, 2);
        if (dst >= s->n_sectors || (size_t)off + len > SECTOR_SIZE || len > n - WRITE_HDR)
            return fail(s, "bad write fragment");
        memcpy(s->enc + off, pay + WRITE_HDR, len);
    } else if (t == MSG_STEP_WRITE_END) {
        msg_step_write_end_t we;
        if (n < sizeof we) return fail(s, "short STEP_WRITE_END (%zu B)", n);
        memcpy(&we, pay, sizeof we);
        if (we.dst >= s->n_sectors || we.enc_len > SECTOR_SIZE ||
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
        stage_sector(s, we.dst);
        /* Only a staged descriptor closes the plan; a NAKed one may be resent. */
        if (we.dst == DESC_SECTOR && s->done[DESC_SECTOR]) s->desc_seen = 1;
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
    case S_TABLE:
        return on_table(s, t, pay, n);
    case S_PLAN:
        return on_step(s, t, pay, n);
    case S_COMMIT:
        return on_commit(s, t);
    default:
        return fail(s, "session already over");
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
    m.active_slot    = m.pending_slot;
    m.pending_slot   = SLOT_NONE;
    m.plan_state     = PLAN_COMMITTED;
    m.attempts_left  = 0;
    m.last_result    = RES_OK;
    return meta_store(f, &m);
}
