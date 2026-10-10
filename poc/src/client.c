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
#include <unistd.h>

static const char *opt_flash = "flash.bin";
static const char *opt_host  = "127.0.0.1";
static int opt_port = 7700;
static int opt_fail_after = -1;

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

static const char *state_name(int s)
{
    static const char *n[] = { "NONE", "STAGING", "READY", "APPLYING",
                               "TRIAL", "COMMITTED" };
    return (s >= 0 && s <= 5) ? n[s] : "?";
}

static uint32_t slot_version(flash_t *f, int slot)
{
    const image_desc_t *d = image_desc(flash_at(f, SLOT_BASE(slot)));
    return d->magic == IMG_MAGIC ? d->version : 0;
}

static void write_sector(flash_t *f, int slot, int index, const uint8_t *data)
{
    int page = (slot == 0 ? SLOT0_PAGE : SLOT1_PAGE) + index;
    flash_erase_page(f, page);
    flash_program(f, (size_t)page * PAGE_SIZE, data, PAGE_SIZE);
}

static void do_unroll(flash_t *f, meta_t *m, const char *why)
{
    printf("boot: unroll (%s)\n", why);
    if (m->pending_slot < SLOT_COUNT) {
        /* Erase sector 0 of the pending slot: the descriptor disappears, so
         * the slot is unmistakably invalid. O(1), nothing to undo. */
        flash_erase_page(f, (m->pending_slot == 0 ? SLOT0_PAGE : SLOT1_PAGE));
    }
    m->pending_slot = 0xFF;
    m->plan_state   = PLAN_NONE;
    m->try_count    = 0;
    m->last_result  = RES_UNROLLED;
    meta_store(f, m);
    printf("boot: active slot %u version %u unchanged\n",
           m->active_slot, slot_version(f, m->active_slot));
}

/* ---------------------------------------------------------------- session */

static int cmd_session(void)
{
    flash_t f;
    meta_t m;
    link_t l;
    manifest_t man;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    uint8_t dev_key[32], root_key[32];

    poc_dev_key(dev_key);
    poc_root_key(root_key);

    if (flash_open(&f, opt_flash, opt_fail_after) < 0) die("cannot open %s", opt_flash);
    identity_t *id = identity_get(&f);
    meta_load(&f, &m);

    double t_start = now_ms(), t_manifest = 0, t_auth = 0, t_stage = 0, t_persist = 0;

    int fd = tcp_connect(opt_host, opt_port);
    if (fd < 0) die("cannot connect to %s:%d", opt_host, opt_port);
    link_init(&l, fd);

    msg_hello_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.device_id, id->device_id, 16);
    h.sector_size     = PAGE_SIZE;
    h.slot_count      = SLOT_COUNT;
    h.sectors_slot    = SECTORS_SLOT;
    h.active_slot     = m.active_slot;
    h.last_result     = (uint8_t)m.last_result;
    h.running_version = slot_version(&f, m.active_slot);
    frame_send(&l, MSG_HELLO, &h, sizeof h);

    if (frame_expect(&l, MSG_CHALLENGE, pay, &n) < 0) die("no CHALLENGE");
    msg_challenge_t ch;
    memcpy(&ch, pay, sizeof ch);

    double t0 = now_ms();
    manifest_build(&f, m.active_slot, &man);
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

    if (t == MSG_NO_UPDATE)        { printf("device: no update offered\n"); goto done; }
    if (t == MSG_MANIFEST_REJECT)  { printf("device: manifest rejected, reason %u\n", pay[0]); goto done; }
    if (t != MSG_PLAN_BEGIN)       die("protocol: expected PLAN_BEGIN, got %d", t);

    msg_plan_begin_t pb;
    memcpy(&pb, pay, sizeof pb);
    printf("device: plan v%u into slot %u, %u steps\n", pb.version, pb.dst_slot, pb.step_count);

    meta_t np;
    meta_init(&np);
    np.generation   = m.generation;
    np.active_slot  = m.active_slot;
    np.pending_slot = pb.dst_slot;
    np.plan_state   = PLAN_STAGING;
    np.target_version = pb.version;
    np.step_count   = pb.step_count;
    memcpy(np.target_sha256, pb.image_sha256, SHA256_LEN);
    memcpy(np.target_mac, pb.image_mac, SHA256_LEN);

    uint8_t sect[PAGE_SIZE];
    uint8_t done[SECTORS_SLOT];
    memset(done, 0, sizeof done);
    int pending_copies = 0;
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
            np.src_slot[c.dst]  = c.src_slot;
            np.src_index[c.dst] = c.src_index;
            memcpy(np.target_md5[c.dst], c.md5, MD5_LEN);
            pending_copies++;
            msg_step_ack_t a = { c.dst, 0 };
            frame_send(&l, MSG_STEP_ACK, &a, sizeof a);
        } else if (t == MSG_STEP_WRITE) {
            uint16_t off, len;
            uint8_t dst = pay[0];
            memcpy(&off, pay + 1, 2);
            memcpy(&len, pay + 3, 2);
            if (dst >= SECTORS_SLOT || (size_t)off + len > PAGE_SIZE)
                die("bad write fragment");
            memcpy(sect + off, pay + 5, len);
        } else if (t == MSG_STEP_WRITE_END) {
            msg_step_write_end_t we;
            memcpy(&we, pay, sizeof we);
            uint8_t got[MD5_LEN];
            md5(sect, PAGE_SIZE, got);
            if (memcmp(got, we.md5, MD5_LEN)) {
                uint8_t nak[2] = { we.dst, NAK_MD5_MISMATCH };
                frame_send(&l, MSG_STEP_NAK, nak, 2);
                printf("device: sector %u md5 mismatch\n", we.dst);
                continue;
            }
            write_sector(&f, pb.dst_slot, we.dst, sect);
            flash_sector_md5(&f, pb.dst_slot, we.dst, got);
            if (memcmp(got, we.md5, MD5_LEN)) {
                uint8_t nak[2] = { we.dst, NAK_FLASH_FAIL };
                frame_send(&l, MSG_STEP_NAK, nak, 2);
                continue;
            }
            md5_bytes += 2 * PAGE_SIZE;   /* hash of the buffer and of the page */
            np.op[we.dst] = OP_WRITE;
            memcpy(np.target_md5[we.dst], we.md5, MD5_LEN);
            done[we.dst] = 1;
            msg_step_ack_t a = { we.dst, 0 };
            frame_send(&l, MSG_STEP_ACK, &a, sizeof a);
        } else {
            die("protocol: unexpected message %d during plan", t);
        }
    }

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
    return 0;
}

/* ------------------------------------------------------------- bootloader */

static int cmd_boot(void)
{
    flash_t f;
    meta_t m;
    uint8_t root_key[32];
    const char *why;

    poc_root_key(root_key);
    if (flash_open(&f, opt_flash, opt_fail_after) < 0) die("cannot open %s", opt_flash);
    double t_start = now_ms(), t_apply = 0, t_verify = 0;
    if (meta_load(&f, &m) < 0) printf("boot: no valid meta bank, assuming slot 0\n");

    if (m.plan_state == PLAN_STAGING) {
        if (journal_get(&f, JRN_ARM_INDEX) != 0x0000) {
            do_unroll(&f, &m, "plan was never committed");
            goto boot_active;
        }
        printf("boot: applying plan v%u into slot %u\n", m.target_version, m.pending_slot);
        double t0 = now_ms();

        for (int i = 0; i < SECTORS_SLOT; i++) {
            if (m.op[i] == OP_NONE) continue;
            if (journal_get(&f, i) == JRN_DONE) continue;

            uint8_t got[MD5_LEN];
            if (m.op[i] == OP_COPY) {
                const uint8_t *src = flash_at(&f, SLOT_BASE(m.src_slot[i]) +
                                              (size_t)m.src_index[i] * PAGE_SIZE);
                md5(src, PAGE_SIZE, got);
                if (memcmp(got, m.target_md5[i], MD5_LEN)) {
                    do_unroll(&f, &m, "copy source hash changed");
                    goto boot_active;
                }
                uint8_t tmp[PAGE_SIZE];
                memcpy(tmp, src, PAGE_SIZE);
                write_sector(&f, m.pending_slot, i, tmp);
            }
            flash_sector_md5(&f, m.pending_slot, i, got);
            if (memcmp(got, m.target_md5[i], MD5_LEN)) {
                do_unroll(&f, &m, "staged sector hash mismatch");
                goto boot_active;
            }
            journal_set(&f, i, JRN_DONE);
            md5_bytes += 2 * PAGE_SIZE;
        }
        t_apply = now_ms() - t0;

        t0 = now_ms();
        uint8_t h[SHA256_LEN];
        const uint8_t *slot = flash_at(&f, SLOT_BASE(m.pending_slot));
        image_hash(slot, h);
        sha_bytes += SLOT_SIZE - DESC_LEN;
        ecdsa_ms += ECDSA_VERIFY_MS;
        /* One hash, two checks: against the plan, then against the descriptor. */
        if (memcmp(h, m.target_sha256, SHA256_LEN)) {
            t_verify = now_ms() - t0;
            do_unroll(&f, &m, "image_sha256 mismatch");
            goto boot_active;
        }
        if (image_verify_hashed(slot, root_key, h, &why) < 0) {
            t_verify = now_ms() - t0;
            do_unroll(&f, &m, why);
            goto boot_active;
        }
        t_verify = now_ms() - t0;
        printf("boot: slot %u verified, image_sha256 and release signature good\n",
               m.pending_slot);

        /* No meta_store here: the TRIAL block below stores once, after
         * taking the first attempt token. Each store costs a 4-page bank
         * erase, which is ~108 ms on the part. */
        m.plan_state = PLAN_TRIAL;
        m.try_count  = MAX_TRIES;
    }

    if (m.plan_state == PLAN_TRIAL) {
        if (m.try_count == 0) {
            printf("boot: trial boots exhausted for version %u\n", m.target_version);
            uint8_t act = m.active_slot;
            do_unroll(&f, &m, "revert: new image never confirmed");
            m.last_result = RES_TRIAL_FAILED;
            m.active_slot = act;
            meta_store(&f, &m);
            goto boot_active;
        }
        m.try_count--;
        meta_store(&f, &m);
        printf("boot: TRIAL slot %u version %u, %u attempts left\n",
               m.pending_slot, slot_version(&f, m.pending_slot), m.try_count);
        printf("boot: jumping to slot %u (run `confirm` if the self-test passes)\n",
               m.pending_slot);
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

boot_active:
    {
        const uint8_t *slot = flash_at(&f, SLOT_BASE(m.active_slot));
        uint8_t h[SHA256_LEN];
        image_hash(slot, h);
        if (image_verify(slot, root_key, &why) < 0) {
            printf("boot: active slot %u is not bootable (%s)\n", m.active_slot, why);
            flash_close(&f);
            return 1;
        }
        printf("boot: booting slot %u version %u\n",
               m.active_slot, slot_version(&f, m.active_slot));
        sha_bytes += SLOT_SIZE - DESC_LEN;   /* image_sha256 on every boot */
    }
    time_report("boot", now_ms() - t_start, &f);
    flash_close(&f);
    return 0;
}

static int cmd_confirm(void)
{
    flash_t f;
    meta_t m;

    if (flash_open(&f, opt_flash, -1) < 0) die("cannot open %s", opt_flash);
    meta_load(&f, &m);
    if (m.plan_state != PLAN_TRIAL) {
        printf("confirm: nothing on trial (state %s)\n", state_name(m.plan_state));
        flash_close(&f);
        return 1;
    }
    m.active_slot  = m.pending_slot;
    m.pending_slot = 0xFF;
    m.plan_state   = PLAN_COMMITTED;
    m.try_count    = 0;
    m.last_result  = RES_OK;
    meta_store(&f, &m);
    printf("confirm: slot %u version %u is now active\n",
           m.active_slot, slot_version(&f, m.active_slot));
    flash_close(&f);
    return 0;
}

static int cmd_unroll(void)
{
    flash_t f;
    meta_t m;
    if (flash_open(&f, opt_flash, -1) < 0) die("cannot open %s", opt_flash);
    meta_load(&f, &m);
    do_unroll(&f, &m, "requested");
    flash_close(&f);
    return 0;
}

static int cmd_status(void)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN];
    char hex[80];

    if (flash_open(&f, opt_flash, -1) < 0) die("cannot open %s", opt_flash);
    identity_t *id = identity_get(&f);
    meta_load(&f, &m);

    hexdump_line(id->device_id, 8, hex);
    printf("device_id      %s...\n", hex);
    printf("meta gen %u  state %s  active %u  pending %u  tries %u  last_result %u\n",
           m.generation, state_name(m.plan_state), m.active_slot, m.pending_slot,
           m.try_count, m.last_result);
    for (int s = 0; s < SLOT_COUNT; s++) {
        const uint8_t *slot = flash_at(&f, SLOT_BASE(s));
        const image_desc_t *dd = image_desc(slot);
        const char *why;
        uint8_t rk[32];
        poc_root_key(rk);
        int ok = image_verify(slot, rk, &why);
        printf("slot %d  %s  version %u  %s\n", s,
               dd->magic == IMG_MAGIC ? "image  " : "no image",
               dd->magic == IMG_MAGIC ? dd->version : 0,
               ok == 0 ? "verified" : why);
    }
    manifest_build(&f, m.active_slot, &man);
    manifest_digest(&man, d);
    hexdump_line(d, 8, hex);
    printf("manifest digest %s...  body %zu B\n", hex, MANIFEST_BODY_LEN);
    flash_close(&f);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc)           opt_flash = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)       opt_host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)       opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fail-after") && i + 1 < argc) opt_fail_after = atoi(argv[++i]);
        else die("unknown option %s", argv[i]);
    }

    if (!strcmp(cmd, "session")) return cmd_session();
    if (!strcmp(cmd, "boot"))    return cmd_boot();
    if (!strcmp(cmd, "confirm")) return cmd_confirm();
    if (!strcmp(cmd, "unroll"))  return cmd_unroll();
    if (!strcmp(cmd, "status"))  return cmd_status();

    fprintf(stderr, "usage: cb-client {session|boot|confirm|unroll|status} [options]\n");
    return 2;
}
