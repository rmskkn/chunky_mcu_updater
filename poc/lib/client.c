/* The device's functional core: manifest generation, the update session
 * (DESIGN.md sections 4, 6), and the bootloader's apply/verify step
 * (sections 7, 8). No argv parsing, no printf: status goes through the log
 * callback, timing goes through client_update_stats_t. cb-client.c is the
 * thin CLI shell around this. */
#include "client.h"
#include "frame.h"
#include "proto.h"
#include "manifest.h"
#include "image.h"
#include "compress.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <pthread.h>

/* Opens the device, creating it on first use with the firmware it shipped
 * with in slot 0. The manifest the client sends is a statement about real
 * flash contents, so the device has to have some. */
void device_open_or_create(flash_t *f, const char *flash_path,
                           const char *initial_path, const uint8_t root_key[32],
                           log_fn log, void *log_ctx)
{
    if (flash_open(f, flash_path) == 0) return;
    if (errno != ENOENT) die("cannot open %s: %s", flash_path, strerror(errno));
    if (!initial_path)
        die("%s does not exist\n"
            "  pass --initial FILE: the firmware the device shipped with,\n"
            "  which is what its first manifest describes",
            flash_path);

    size_t binlen;
    uint8_t *bin = read_file(initial_path, &binlen);
    if (!bin) die("cannot read %s", initial_path);
    if (flash_create(flash_path) < 0) die("cannot create %s", flash_path);
    /* Seed with the fault injection and the counters off: this is the factory
     * programmer's work, not the update's, and it must not show up in either
     * the timings or --fail-after. */
    if (flash_open(f, flash_path) < 0) die("cannot open %s", flash_path);

    if (image_install(f, bin, binlen, 1, root_key) < 0)
        die("%s is %zu B, over the %d B slot payload", initial_path, binlen, IMG_MAX_PAYLOAD);
    free(bin);
    log_fmt(log, log_ctx, "device: created %s with %s as version 1", flash_path, initial_path);

    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
}

uint32_t client_app_version(flash_t *f)
{
    const image_desc_t *d = image_desc(flash_at(f, APP_BASE));
    return d->magic == IMG_MAGIC ? d->version : 0;
}

/* Double-buffered flash write: one sector programs on this worker while the
 * main thread keeps draining the socket into the other buffer, so the
 * (modelled) erase/program time overlaps the next sector's transfer instead
 * of stalling it. Only one job is ever in flight - a real part has one flash
 * controller - so a new job joins the previous one before it starts, which
 * is also what gives the run its backpressure if flashing falls behind the
 * link. The job owns dst's slot in np/done; the main thread must join
 * before it reads or writes that slot itself. */
typedef struct {
    pthread_t   tid;
    int         active;
    flash_t     *f;
    link_t      *l;
    meta_t      *np;
    uint8_t     *done;
    sector_idx_t dst;
    const uint8_t *buf;
    uint8_t     hash[SECTOR_HASH_LEN];
    client_update_stats_t *stats;
} flash_job_t;

static pthread_mutex_t io_lock = PTHREAD_MUTEX_INITIALIZER;

static void *flash_worker(void *arg)
{
    flash_job_t *j = arg;
    uint8_t got[SECTOR_HASH_LEN];

    /* The sector may already hold this exact content - a stale manifest
     * cache or a re-sent step, say. Check before erasing: an erase/program
     * cycle is real wear and the slow part of this job, a hash is neither. */
    flash_sector_hash(j->f, j->dst, got);
    if (memcmp(got, j->hash, SECTOR_HASH_LEN)) {
        flash_write_app_sector(j->f, j->dst, j->buf);
        flash_sector_hash(j->f, j->dst, got);
    }

    msg_step_ack_t a = { j->dst, 0 };
    pthread_mutex_lock(&io_lock);
    if (memcmp(got, j->hash, SECTOR_HASH_LEN)) {
        a.code = NAK_FLASH_FAIL;
        frame_send(j->l, MSG_STEP_NAK, &a, sizeof a);
    } else {
        j->np->op[j->dst] = OP_WRITE;
        memcpy(j->np->target_hash[j->dst], j->hash, SECTOR_HASH_LEN);
        j->done[j->dst] = 1;
        j->stats->sha_bytes += 2 * SECTOR_SIZE;   /* hash of the buffer and of the sector in flash */
        frame_send(j->l, MSG_STEP_ACK, &a, sizeof a);
    }
    pthread_mutex_unlock(&io_lock);
    return NULL;
}

/* Verifies the decoded buffer against the sender's hash, then hands it to
 * the flash worker and flips to the other buffer. */
static void stage_sector(link_t *l, meta_t *np, uint8_t *done, flash_job_t *job,
                          flash_t *f, uint8_t sect[2][SECTOR_SIZE], int *cur,
                          sector_idx_t dst, const uint8_t hash[SECTOR_HASH_LEN],
                          client_update_stats_t *stats, log_fn log, void *log_ctx)
{
    uint8_t got[SECTOR_HASH_LEN];
    sector_hash(sect[*cur], got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        msg_step_ack_t nak = { dst, NAK_HASH_MISMATCH };
        pthread_mutex_lock(&io_lock);
        frame_send(l, MSG_STEP_NAK, &nak, sizeof nak);
        pthread_mutex_unlock(&io_lock);
        log_fmt(log, log_ctx, "device: sector %u hash mismatch", dst);
        return;
    }

    if (job->active) { pthread_join(job->tid, NULL); job->active = 0; }
    job->f = f; job->l = l; job->np = np; job->done = done;
    job->dst = dst; job->buf = sect[*cur]; job->stats = stats;
    memcpy(job->hash, hash, SECTOR_HASH_LEN);
    job->active = 1;
    pthread_create(&job->tid, NULL, flash_worker, job);
    *cur = 1 - *cur;
}

int client_update_session(flash_t *f, int fd, const uint8_t dev_key[32],
                          log_fn log, void *log_ctx, client_update_stats_t *stats)
{
    client_update_stats_t dummy = { 0 };
    if (!stats) stats = &dummy;
    stats->t_manifest = stats->t_auth = stats->t_stage = stats->t_persist = 0;

    meta_t m;
    link_t l;
    manifest_t man;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    int rc = 0;

    identity_t *id = identity_get(f);
    meta_load(f, &m);

    link_init(&l, fd);

    msg_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, id->device_id, 16);
    h.sector_size     = SECTOR_SIZE;
    h.sectors         = SECTORS_SLOT;
    h.last_result     = (uint8_t)m.last_result;
    h.running_version = client_app_version(f);
    {
        const uint8_t *app = flash_at(f, APP_BASE);
        if (image_desc(app)->magic == IMG_MAGIC) image_hash(app, h.running_sha256);
    }
    frame_send(&l, MSG_HELLO, &h, sizeof h);

    if (frame_expect(&l, MSG_CHALLENGE, pay, &n) < 0) die("no CHALLENGE");
    msg_challenge_t ch;
    memcpy(&ch, pay, sizeof ch);

    double t0 = now_ms();
    manifest_build(f, &man);
    stats->t_manifest = now_ms() - t0;
    stats->sha_bytes += (size_t)MANIFEST_ENTRIES * SECTOR_SIZE;   /* both slots hashed */

    t0 = now_ms();
    msg_manifest_auth_t auth;
    manifest_digest(&man, auth.manifest_sha256);
    manifest_mac(dev_key, id->device_id, ch.nonce, auth.manifest_sha256, auth.mac);
    stats->t_auth = now_ms() - t0;
    stats->sha_bytes += MANIFEST_BODY_LEN;
    stats->ecdsa_ms += ECDSA_SIGN_MS;        /* the real signature, not this HMAC */
    frame_send(&l, MSG_MANIFEST_DIGEST, &auth, sizeof auth);

    int t = frame_recv(&l, pay, &n);
    if (t == MSG_MANIFEST_NEEDED) {
        const uint8_t *body = (const uint8_t *)&man;
        for (size_t off = 0; off < MANIFEST_BODY_LEN; off += 256) {
            uint8_t buf[4 + 256];
            uint16_t len = (uint16_t)(MANIFEST_BODY_LEN - off < 256 ? MANIFEST_BODY_LEN - off : 256);
            uint16_t o16 = (uint16_t)off;
            memcpy(buf, &o16, 2);
            memcpy(buf + 2, &len, 2);
            memcpy(buf + 4, body + off, len);
            frame_send(&l, MSG_MANIFEST, buf, 4 + len);
        }
        frame_send(&l, MSG_MANIFEST_END, &auth, sizeof auth);
        log_fmt(log, log_ctx, "device: full manifest sent (%zu B of body)", MANIFEST_BODY_LEN);
        t = frame_recv(&l, pay, &n);
    } else {
        log_fmt(log, log_ctx, "device: manifest digest accepted, body elided");
    }

    if (t == MSG_NO_UPDATE)        { log_fmt(log, log_ctx, "device: no update offered"); rc = 1; goto done; }
    if (t == MSG_MANIFEST_REJECT)  { log_fmt(log, log_ctx, "device: manifest rejected, reason %u", pay[0]); rc = 1; goto done; }
    if (t != MSG_PLAN_BEGIN)       die("protocol: expected PLAN_BEGIN, got %d", t);

    msg_plan_begin_t pb;
    memcpy(&pb, pay, sizeof pb);
    log_fmt(log, log_ctx, "device: plan v%u, %u steps", pb.version, pb.step_count);

    meta_t np;
    meta_init(&np);
    np.generation   = m.generation;
    np.plan_state   = PLAN_STAGING;
    np.target_version = pb.version;
    np.step_count   = pb.step_count;
    memcpy(np.target_sha256, pb.image_sha256, SHA256_LEN);
    memcpy(np.target_mac, pb.image_mac, SHA256_LEN);

    uint8_t sect[2][SECTOR_SIZE];
    uint8_t enc[SECTOR_SIZE];   /* the encoded sector being assembled */
    uint8_t done[SECTORS_SLOT];
    memset(done, 0, sizeof done);
    int pending_copies = 0;
    int cur = 0;                /* buffer the next STEP_WRITE fills */
    flash_job_t job = { .active = 0 };
    t0 = now_ms();

    for (;;) {
        t = frame_recv(&l, pay, &n);
        if (t < 0) die("link closed mid-plan");
        if (t == MSG_PLAN_END) break;

        if (t == MSG_STEP_COPY) {
            msg_step_copy_t c;
            memcpy(&c, pay, sizeof c);
            if (c.dst >= SECTORS_SLOT) die("bad sector index %u", c.dst);
            /* Recorded only. The bootloader does flash-to-flash copies
             * (DESIGN.md section 7 step 2). */
            np.op[c.dst]        = OP_COPY;
            np.src_index[c.dst] = c.src_index;
            memcpy(np.target_hash[c.dst], c.hash, SECTOR_HASH_LEN);
            pending_copies++;
            msg_step_ack_t a = { c.dst, 0 };
            pthread_mutex_lock(&io_lock);
            frame_send(&l, MSG_STEP_ACK, &a, sizeof a);
            pthread_mutex_unlock(&io_lock);
        } else if (t == MSG_STEP_WRITE) {
            sector_idx_t dst;
            uint16_t off, len;
            memcpy(&dst, pay + 0, 2);
            memcpy(&off, pay + 2, 2);
            memcpy(&len, pay + 4, 2);
            if (dst >= SECTORS_SLOT || (size_t)off + len > SECTOR_SIZE)
                die("bad write fragment");
            memcpy(enc + off, pay + WRITE_HDR, len);
        } else if (t == MSG_STEP_WRITE_END) {
            msg_step_write_end_t we;
            memcpy(&we, pay, sizeof we);
            if (we.dst >= SECTORS_SLOT || we.enc_len > SECTOR_SIZE ||
                (we.compress == COMPRESS_RAW && we.enc_len != SECTOR_SIZE))
                die("bad write end");

            /* The dictionary is dst's current flash content, which is what our
             * manifest told the server it holds: dst is written once per plan,
             * and only after this. Not while the worker may be rewriting it. */
            if (job.active && job.dst == we.dst) { pthread_join(job.tid, NULL); job.active = 0; }
            if (decompress_sector(we.compress, enc, we.enc_len,
                                  flash_at(f, APP_BASE + (size_t)we.dst * SECTOR_SIZE),
                                  sect[cur]) < 0) {
                msg_step_ack_t nak = { we.dst, NAK_DECOMPRESS };
                pthread_mutex_lock(&io_lock);
                frame_send(&l, MSG_STEP_NAK, &nak, sizeof nak);
                pthread_mutex_unlock(&io_lock);
                log_fmt(log, log_ctx, "device: sector %u does not decompress", we.dst);
                continue;
            }
            stage_sector(&l, &np, done, &job, f, sect, &cur, we.dst, we.hash, stats, log, log_ctx);
        } else {
            die("protocol: unexpected message %d during plan", t);
        }
    }

    /* Every STEP_WRITE_END's job must land - and its ACK/NAK be sent, and its
     * np/done slot be written - before np/done are read below. */
    if (job.active) { pthread_join(job.tid, NULL); job.active = 0; }

    stats->t_stage = now_ms() - t0;

    /* Persist the plan once, then mark the sectors already in flash.
     * meta_store() erases the target bank, so it must come first. */
    t0 = now_ms();
    meta_store(f, &np);
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (done[i]) journal_set(f, i, JRN_VERIFIED);
    stats->t_persist = now_ms() - t0;

    msg_plan_ready_t pr = { (uint16_t)pending_copies };
    frame_send(&l, MSG_PLAN_READY, &pr, sizeof pr);

    t = frame_recv(&l, pay, &n);
    if (t == MSG_COMMIT) {
        journal_set(f, JRN_ARM_INDEX, 0x0000);   /* arm: plan is committed */
        log_fmt(log, log_ctx, "device: plan armed, %d copies left for the bootloader; reboot to apply",
               pending_copies);
    } else {
        log_fmt(log, log_ctx, "device: no COMMIT, plan left unarmed and will be unrolled at boot");
    }

done:
    log_fmt(log, log_ctx, "device: wire tx=%llu rx=%llu bytes",
           (unsigned long long)l.tx_bytes, (unsigned long long)l.rx_bytes);
    return rc;
}

/* Applies whatever the session staged, then decides whether the partition is
 * bootable. With one partition there is nothing to fall back to: a failure is
 * reported and the next request is the recovery path. */
void client_update_boot(flash_t *f, const uint8_t root_key[32],
                        log_fn log, void *log_ctx, client_update_stats_t *stats)
{
    client_update_stats_t dummy = { 0 };
    if (!stats) stats = &dummy;
    stats->t_apply = stats->t_verify = 0;

    meta_t m;
    const char *why = NULL;

    meta_load(f, &m);

    if (m.plan_state == PLAN_STAGING && journal_get(f, JRN_ARM_INDEX) != 0x0000) {
        log_fmt(log, log_ctx, "boot: plan was never committed, discarding it");
        m.plan_state  = PLAN_NONE;
        m.last_result = RES_VERIFY_FAILED;
        meta_store(f, &m);
    } else if (m.plan_state == PLAN_STAGING) {
        log_fmt(log, log_ctx, "boot: applying plan v%u", m.target_version);
        double t0 = now_ms();
        for (int i = 0; i < SECTORS_SLOT; i++) {
            if (m.op[i] == OP_NONE || journal_get(f, i) == JRN_DONE) continue;

            uint8_t got[SECTOR_HASH_LEN];
            if (m.op[i] == OP_COPY) {
                /* Flash to flash inside the partition, through RAM. */
                uint8_t tmp[SECTOR_SIZE];
                memcpy(tmp, flash_at(f, APP_BASE +
                       (size_t)m.src_index[i] * SECTOR_SIZE), SECTOR_SIZE);
                sector_hash(tmp, got);
                if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                    log_fmt(log, log_ctx, "boot: copy source for sector %d no longer matches", i);
                    m.plan_state = PLAN_BAD;
                    break;
                }
                flash_write_app_sector(f, i, tmp);
            }
            flash_sector_hash(f, i, got);
            if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                log_fmt(log, log_ctx, "boot: sector %d does not match the plan", i);
                m.plan_state = PLAN_BAD;
                break;
            }
            journal_set(f, i, JRN_DONE);
            stats->sha_bytes += 2 * SECTOR_SIZE;
        }
        stats->t_apply = now_ms() - t0;

        if (m.plan_state != PLAN_BAD) {
            t0 = now_ms();
            uint8_t hsh[SHA256_LEN];
            const uint8_t *app = flash_at(f, APP_BASE);
            image_hash(app, hsh);
            stats->sha_bytes += SLOT_SIZE - DESC_LEN;
            stats->ecdsa_ms += ECDSA_VERIFY_MS;
            if (memcmp(hsh, m.target_sha256, SHA256_LEN) ||
                image_verify_hashed(app, root_key, hsh, &why) < 0) {
                log_fmt(log, log_ctx, "boot: the staged image does not verify (%s)",
                       why ? why : "image_sha256 mismatch");
                m.plan_state = PLAN_BAD;
            } else {
                m.plan_state  = PLAN_COMMITTED;
                m.last_result = RES_OK;
            }
            stats->t_verify = now_ms() - t0;
        }
        meta_store(f, &m);
    }

    {
        const uint8_t *app = flash_at(f, APP_BASE);
        uint8_t hsh[SHA256_LEN];
        image_hash(app, hsh);
        stats->sha_bytes += SLOT_SIZE - DESC_LEN;
        if (image_verify_hashed(app, root_key, hsh, &why) < 0) {
            /* No second partition, so this is as far as the device gets: it
             * keeps asking for a release until one verifies. */
            log_fmt(log, log_ctx, "boot: no bootable image (%s); requesting an update", why);
            if (m.plan_state != PLAN_BAD) {
                m.plan_state  = PLAN_BAD;
                m.last_result = RES_VERIFY_FAILED;
                meta_store(f, &m);
            }
        } else {
            log_fmt(log, log_ctx, "boot: booting version %u", client_app_version(f));
        }
    }
}
