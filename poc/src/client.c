/* cb-client: the device. Owns an emulated flash file, runs an update session,
 * then acts as the bootloader that applies, verifies, unrolls and reverts.
 * DESIGN.md sections 4, 6, 7, 8. */
#include "frame.h"
#include "proto.h"
#include "manifest.h"
#include "image.h"
#include "keys.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <pthread.h>

static const char *opt_flash = "flash.bin";
static const char *opt_host  = "127.0.0.1";
static int opt_port = 7700;
static const char *opt_initial = NULL;
static int opt_status = 0;

/* Device-time model. The host runs the same code thousands of times faster,
 * so the interesting number is what the work would cost on the part: hashing
 * at the cycles/byte in flash.h, plus RM0008 flash erase and program times.
 * ECDSA costs are the figures DESIGN.md section 10 budgets, not measurements. */
#define ECDSA_SIGN_MS   250.0
#define ECDSA_VERIFY_MS 300.0

static size_t md5_bytes, sha_bytes;
static double ecdsa_ms;

static double model_ms(const flash_t *f, int worst)
{
    double cyc_per_ms = F103_MHZ * 1000.0;
    return (double)md5_bytes * F103_MD5_CYC_B / cyc_per_ms +
           (double)sha_bytes * F103_SHA256_CYC_B / cyc_per_ms +
           f103_flash_ms(f->n_erase, f->prog_bytes, worst) + ecdsa_ms;
}

static void time_report(const char *phase, double host_ms, const flash_t *f)
{
    printf("time: %-10s host %8.2f ms   F103 %8.1f - %.1f ms"
           "  (flash %.1f - %.1f ms: %ld erase, %zu B prog)\n",
           phase, host_ms, model_ms(f, 0), model_ms(f, 1),
           f103_flash_ms(f->n_erase, f->prog_bytes, 0),
           f103_flash_ms(f->n_erase, f->prog_bytes, 1),
           f->n_erase, f->prog_bytes);
    if (!machine_output()) { fflush(stdout); return; }
    printf("TIME phase=%s host_ms=%.3f model_min_ms=%.1f model_max_ms=%.1f "
           "flash_min_ms=%.1f flash_max_ms=%.1f erases=%ld "
           "prog_bytes=%zu md5_bytes=%zu sha_bytes=%zu ecdsa_ms=%.0f\n",
           phase, host_ms, model_ms(f, 0), model_ms(f, 1),
           f103_flash_ms(f->n_erase, f->prog_bytes, 0),
           f103_flash_ms(f->n_erase, f->prog_bytes, 1),
           f->n_erase, f->prog_bytes,
           md5_bytes, sha_bytes, ecdsa_ms);
    fflush(stdout);
}

/* Opens the device, creating it on first use with the firmware it shipped
 * with in slot 0. The manifest the client sends is a statement about real
 * flash contents, so the device has to have some. */
static void open_device(flash_t *f)
{
    if (flash_open(f, opt_flash) == 0) return;
    if (errno != ENOENT) die("cannot open %s: %s", opt_flash, strerror(errno));
    if (!opt_initial)
        die("%s does not exist\n"
            "  pass --initial FILE: the firmware the device shipped with,\n"
            "  which is what its first manifest describes",
            opt_flash);

    size_t binlen;
    uint8_t *bin = read_file(opt_initial, &binlen);
    if (!bin) die("cannot read %s", opt_initial);
    if (flash_create(opt_flash) < 0) die("cannot create %s", opt_flash);
    /* Seed with the fault injection and the counters off: this is the factory
     * programmer's work, not the update's, and it must not show up in either
     * the timings or --fail-after. */
    if (flash_open(f, opt_flash) < 0) die("cannot open %s", opt_flash);

    uint8_t root_key[32];
    poc_root_key(root_key);
    if (image_install(f, bin, binlen, 1, root_key) < 0)
        die("%s is %zu B, over the %d B slot payload", opt_initial, binlen, IMG_MAX_PAYLOAD);
    free(bin);
    printf("device: created %s with %s as version 1\n", opt_flash, opt_initial);

    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
}

static const char *state_name(int st)
{
    static const char *n[] = { "NONE", "STAGING", "APPLYING", "COMMITTED", "BAD" };
    return (st >= 0 && st <= 4) ? n[st] : "?";
}

static uint32_t app_version(flash_t *f)
{
    const image_desc_t *d = image_desc(flash_at(f, APP_BASE));
    return d->magic == IMG_MAGIC ? d->version : 0;
}

static void write_sector(flash_t *f, int index, const uint8_t *data)
{
    flash_erase_page(f, APP_PAGE + index);
    flash_program(f, (size_t)(APP_PAGE + index) * PAGE_SIZE, data, PAGE_SIZE);
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
        write_sector(j->f, j->dst, j->buf);
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
        md5_bytes += 2 * PAGE_SIZE;   /* hash of the buffer and of the page */
        frame_send(j->l, MSG_STEP_ACK, &a, sizeof a);
    }
    pthread_mutex_unlock(&io_lock);
    return NULL;
}

/* Verifies the assembled buffer against the sender's hash, then hands it to
 * the flash worker and flips to the other buffer. Shared by STEP_WRITE_END
 * and STEP_FILL - both close out one sector the same way. */
static void stage_sector(link_t *l, meta_t *np, uint8_t *done, flash_job_t *job,
                          flash_t *f, uint8_t sect[2][PAGE_SIZE], int *cur,
                          sector_idx_t dst, const uint8_t hash[SECTOR_HASH_LEN])
{
    uint8_t got[SECTOR_HASH_LEN];
    sector_hash(sect[*cur], got);
    if (memcmp(got, hash, SECTOR_HASH_LEN)) {
        msg_step_ack_t nak = { dst, NAK_MD5_MISMATCH };
        pthread_mutex_lock(&io_lock);
        frame_send(l, MSG_STEP_NAK, &nak, sizeof nak);
        pthread_mutex_unlock(&io_lock);
        printf("device: sector %u md5 mismatch\n", dst);
        return;
    }

    if (job->active) { pthread_join(job->tid, NULL); job->active = 0; }
    job->f = f; job->l = l; job->np = np; job->done = done;
    job->dst = dst; job->buf = sect[*cur];
    memcpy(job->hash, hash, SECTOR_HASH_LEN);
    job->active = 1;
    pthread_create(&job->tid, NULL, flash_worker, job);
    *cur = 1 - *cur;
}

/* ---------------------------------------------------------------- session */

/* 0 = a plan was staged, 1 = nothing to do. */
static int cmd_session(void)
{
    flash_t f;
    meta_t m;
    link_t l;
    manifest_t man;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    uint8_t dev_key[32], root_key[32];
    int rc = 0;

    poc_dev_key(dev_key);
    poc_root_key(root_key);

    open_device(&f);
    identity_t *id = identity_get(&f);
    meta_load(&f, &m);

    double t_start = now_ms(), t_manifest = 0, t_auth = 0, t_stage = 0, t_persist = 0;

    int fd = tcp_connect(opt_host, opt_port);
    if (fd < 0)
        die("cannot connect to %s:%d: %s\n"
            "  no server is listening there; start one in another terminal:\n"
            "    scripts/lab.sh server",
            opt_host, opt_port, strerror(errno));
    link_init(&l, fd);

    msg_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, id->device_id, 16);
    h.sector_size     = PAGE_SIZE;
    h.sectors         = SECTORS_SLOT;
    h.last_result     = (uint8_t)m.last_result;
    h.running_version = app_version(&f);
    {
        const uint8_t *app = flash_at(&f, APP_BASE);
        if (image_desc(app)->magic == IMG_MAGIC) image_hash(app, h.running_sha256);
    }
    frame_send(&l, MSG_HELLO, &h, sizeof h);

    if (frame_expect(&l, MSG_CHALLENGE, pay, &n) < 0) die("no CHALLENGE");
    msg_challenge_t ch;
    memcpy(&ch, pay, sizeof ch);

    double t0 = now_ms();
    manifest_build(&f, &man);
    t_manifest = now_ms() - t0;
    md5_bytes += (size_t)MANIFEST_ENTRIES * PAGE_SIZE;   /* both slots hashed */

    t0 = now_ms();
    msg_manifest_auth_t auth;
    manifest_digest(&man, auth.manifest_sha256);
    manifest_mac(dev_key, id->device_id, ch.nonce, auth.manifest_sha256, auth.mac);
    t_auth = now_ms() - t0;
    sha_bytes += MANIFEST_BODY_LEN;
    ecdsa_ms += ECDSA_SIGN_MS;        /* the real signature, not this HMAC */
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
        printf("device: full manifest sent (%zu B of body)\n", MANIFEST_BODY_LEN);
        t = frame_recv(&l, pay, &n);
    } else {
        printf("device: manifest digest accepted, body elided\n");
    }

    if (t == MSG_NO_UPDATE)        { printf("device: no update offered\n"); rc = 1; goto done; }
    if (t == MSG_MANIFEST_REJECT)  { printf("device: manifest rejected, reason %u\n", pay[0]); rc = 1; goto done; }
    if (t != MSG_PLAN_BEGIN)       die("protocol: expected PLAN_BEGIN, got %d", t);

    msg_plan_begin_t pb;
    memcpy(&pb, pay, sizeof pb);
    printf("device: plan v%u, %u steps\n", pb.version, pb.step_count);

    meta_t np;
    meta_init(&np);
    np.generation   = m.generation;
    np.plan_state   = PLAN_STAGING;
    np.target_version = pb.version;
    np.step_count   = pb.step_count;
    memcpy(np.target_sha256, pb.image_sha256, SHA256_LEN);
    memcpy(np.target_mac, pb.image_mac, SHA256_LEN);

    uint8_t sect[2][PAGE_SIZE];
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
            if (dst >= SECTORS_SLOT || (size_t)off + len > PAGE_SIZE)
                die("bad write fragment");
            memcpy(sect[cur] + off, pay + WRITE_HDR, len);
        } else if (t == MSG_STEP_WRITE_END) {
            msg_step_write_end_t we;
            memcpy(&we, pay, sizeof we);
            stage_sector(&l, &np, done, &job, &f, sect, &cur, we.dst, we.hash);
        } else if (t == MSG_STEP_FILL) {
            msg_step_fill_t fl;
            memcpy(&fl, pay, sizeof fl);
            if (fl.dst >= SECTORS_SLOT) die("bad sector index %u", fl.dst);
            memset(sect[cur], fl.value, PAGE_SIZE);
            stage_sector(&l, &np, done, &job, &f, sect, &cur, fl.dst, fl.hash);
        } else {
            die("protocol: unexpected message %d during plan", t);
        }
    }

    /* Every STEP_WRITE_END's job must land - and its ACK/NAK be sent, and its
     * np/done slot be written - before np/done are read below. */
    if (job.active) { pthread_join(job.tid, NULL); job.active = 0; }

    t_stage = now_ms() - t0;

    /* Persist the plan once, then mark the sectors already in flash.
     * meta_store() erases the target bank, so it must come first. */
    t0 = now_ms();
    meta_store(&f, &np);
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (done[i]) journal_set(&f, i, JRN_VERIFIED);
    t_persist = now_ms() - t0;

    msg_plan_ready_t pr = { (uint16_t)pending_copies };
    frame_send(&l, MSG_PLAN_READY, &pr, sizeof pr);

    t = frame_recv(&l, pay, &n);
    if (t == MSG_COMMIT) {
        journal_set(&f, JRN_ARM_INDEX, 0x0000);   /* arm: plan is committed */
        printf("device: plan armed, %d copies left for the bootloader; reboot to apply\n",
               pending_copies);
    } else {
        printf("device: no COMMIT, plan left unarmed and will be unrolled at boot\n");
    }

done:
    printf("device: wire tx=%llu rx=%llu bytes\n",
           (unsigned long long)l.tx_bytes, (unsigned long long)l.rx_bytes);
    printf("time: manifest   host %8.2f ms   F103 model %8.1f ms  (%d sectors hashed)\n",
           t_manifest, (double)MANIFEST_ENTRIES * PAGE_SIZE * F103_MD5_CYC_B / (F103_MHZ * 1000.0),
           MANIFEST_ENTRIES);
    printf("time: sign       host %8.2f ms   F103 model %8.1f ms  (ECDSA P-256, budgeted)\n",
           t_auth, ECDSA_SIGN_MS);
    printf("time: stage      host %8.2f ms   F103 %8.1f - %.1f ms  (receive, verify, program)\n",
           t_stage, f103_flash_ms(f.n_erase, f.prog_bytes, 0),
           f103_flash_ms(f.n_erase, f.prog_bytes, 1));
    printf("time: persist    host %8.2f ms\n", t_persist);
    time_report("session", now_ms() - t_start, &f);
    close(fd);
    flash_close(&f);
    return rc;
}

/* ------------------------------------------------------------- bootloader */

/* Applies whatever the session staged, then decides whether the partition is
 * bootable. With one partition there is nothing to fall back to: a failure is
 * reported and the next request is the recovery path. */
static int cmd_boot(void)
{
    flash_t f;
    meta_t m;
    uint8_t root_key[32];
    const char *why;

    poc_root_key(root_key);
    open_device(&f);
    double t_start = now_ms(), t_apply = 0, t_verify = 0;
    meta_load(&f, &m);

    if (m.plan_state == PLAN_STAGING && journal_get(&f, JRN_ARM_INDEX) != 0x0000) {
        printf("boot: plan was never committed, discarding it\n");
        m.plan_state  = PLAN_NONE;
        m.last_result = RES_VERIFY_FAILED;
        meta_store(&f, &m);
    } else if (m.plan_state == PLAN_STAGING) {
        printf("boot: applying plan v%u\n", m.target_version);
        double t0 = now_ms();
        for (int i = 0; i < SECTORS_SLOT; i++) {
            if (m.op[i] == OP_NONE || journal_get(&f, i) == JRN_DONE) continue;

            uint8_t got[SECTOR_HASH_LEN];
            if (m.op[i] == OP_COPY) {
                /* Flash to flash inside the partition, through RAM. */
                uint8_t tmp[PAGE_SIZE];
                memcpy(tmp, flash_at(&f, APP_BASE +
                       (size_t)m.src_index[i] * PAGE_SIZE), PAGE_SIZE);
                sector_hash(tmp, got);
                if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                    printf("boot: copy source for sector %d no longer matches\n", i);
                    m.plan_state = PLAN_BAD;
                    break;
                }
                write_sector(&f, i, tmp);
            }
            flash_sector_hash(&f, i, got);
            if (memcmp(got, m.target_hash[i], SECTOR_HASH_LEN)) {
                printf("boot: sector %d does not match the plan\n", i);
                m.plan_state = PLAN_BAD;
                break;
            }
            journal_set(&f, i, JRN_DONE);
            md5_bytes += 2 * PAGE_SIZE;
        }
        t_apply = now_ms() - t0;

        if (m.plan_state != PLAN_BAD) {
            t0 = now_ms();
            uint8_t hsh[SHA256_LEN];
            const uint8_t *app = flash_at(&f, APP_BASE);
            image_hash(app, hsh);
            sha_bytes += SLOT_SIZE - DESC_LEN;
            ecdsa_ms += ECDSA_VERIFY_MS;
            if (memcmp(hsh, m.target_sha256, SHA256_LEN) ||
                image_verify_hashed(app, root_key, hsh, &why) < 0) {
                printf("boot: the staged image does not verify (%s)\n",
                       why ? why : "image_sha256 mismatch");
                m.plan_state = PLAN_BAD;
            } else {
                m.plan_state  = PLAN_COMMITTED;
                m.last_result = RES_OK;
            }
            t_verify = now_ms() - t0;
        }
        meta_store(&f, &m);
    }

    {
        const uint8_t *app = flash_at(&f, APP_BASE);
        uint8_t hsh[SHA256_LEN];
        image_hash(app, hsh);
        sha_bytes += SLOT_SIZE - DESC_LEN;
        if (image_verify_hashed(app, root_key, hsh, &why) < 0) {
            /* No second partition, so this is as far as the device gets: it
             * keeps asking for a release until one verifies. */
            printf("boot: no bootable image (%s); requesting an update\n", why);
            if (m.plan_state != PLAN_BAD) {
                m.plan_state  = PLAN_BAD;
                m.last_result = RES_VERIFY_FAILED;
                meta_store(&f, &m);
            }
        } else {
            printf("boot: booting version %u\n", app_version(&f));
        }
    }

    if (t_apply > 0)
        printf("time: apply      host %8.2f ms   F103 %8.1f - %.1f ms  (copies)\n",
               t_apply, f103_flash_ms(f.n_erase, f.prog_bytes, 0),
               f103_flash_ms(f.n_erase, f.prog_bytes, 1));
    if (t_verify > 0)
        printf("time: verify     host %8.2f ms   F103 %8.1f ms  "
               "(image_sha256 + signature)\n", t_verify,
               (double)(SLOT_SIZE - DESC_LEN) * F103_SHA256_CYC_B / (F103_MHZ * 1000.0)
               + ECDSA_VERIFY_MS);
    time_report("boot", now_ms() - t_start, &f);
    flash_close(&f);
    return 0;
}

static int cmd_status(void)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN], rk[32];
    char hex[80];
    const char *why;

    open_device(&f);
    identity_t *id = identity_get(&f);
    meta_load(&f, &m);
    poc_root_key(rk);

    hexdump_line(id->device_id, 8, hex);
    printf("device_id       %s...\n", hex);
    printf("meta gen %u  state %s  last_result %u\n",
           m.generation, state_name(m.plan_state), m.last_result);

    const uint8_t *app = flash_at(&f, APP_BASE);
    int ok = image_verify(app, rk, &why);
    printf("application     %s  version %u  %s\n",
           image_desc(app)->magic == IMG_MAGIC ? "image   " : "no image",
           app_version(&f), ok == 0 ? "verified" : why);

    manifest_build(&f, &man);
    manifest_digest(&man, d);
    hexdump_line(d, 8, hex);
    printf("manifest digest %s...  %d sectors, body %zu B\n",
           hex, SECTORS_SLOT, MANIFEST_BODY_LEN);
    flash_close(&f);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: cb-client --flash FILE [options]\n"
        "  runs one whole update attempt: send the manifest, take the sectors\n"
        "  the server says differ, apply them, verify, boot and confirm.\n"
        "\n"
        "  --flash FILE       emulated flash device (required)\n"
        "  --initial FILE     firmware to create the device with, first run only\n"
        "  --host H           server address, default 127.0.0.1\n"
        "  --port N           server port, default 7700\n"
        "  --status           print the device state and exit\n");
    return 2;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc)           opt_flash = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)       opt_host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)       opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--initial") && i + 1 < argc)    opt_initial = argv[++i];
        else if (!strcmp(argv[i], "--status"))                     opt_status = 1;
        else return usage();
    }

    if (opt_status) return cmd_status();

    /* Ask for an update by reporting what is in flash, then reboot into the
     * bootloader to apply and verify it. One partition, so the write is in
     * place and there is no trial boot to pass. */
    cmd_session();
    cmd_boot();
    return cmd_status();
}
