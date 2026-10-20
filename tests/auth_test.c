/* Plan authentication and meta integrity (docs/auth_verification.md,
 * docs/auth_implementation_plan.md). Drives lib/device.c's session and
 * lib/bootloader.c directly, message by message, so each case can forge what
 * a real server never would: a tampered signature or table, a step the table
 * does not list, a misordered descriptor sector, a rewritten meta bank.
 *
 *   auth_test WORKDIR BASE.bin
 *
 * WORKDIR gets one flash file per case. BASE.bin is the firmware every
 * device ships with (tests/common.sh's gen_random_bin). */
#include "bootloader.h"
#include "device.h"
#include "flash_file.h"
#include "host.h"
#include "image.h"
#include "keys.h"
#include "plan.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t dev_key[32], root_key[32];
static const char *workdir, *base_path;
static uint8_t *base_bin;
static size_t base_len;

#define REQUIRE(cond, ...) do { \
    if (!(cond)) { printf("FAIL  auth_test: %s: ", __func__); printf(__VA_ARGS__); \
                   printf("\n"); return -1; } } while (0)

/* What the device said and logged during a case. */
typedef struct {
    int  acks, naks;
    int  last_nak;
    char log[8192];
} cap_t;

static int cap_send(void *ctx, uint8_t type, const void *p, size_t n)
{
    cap_t *c = ctx;
    if (type == MSG_STEP_ACK) c->acks++;
    if (type == MSG_STEP_NAK) { c->naks++; c->last_nak = ((const msg_step_ack_t *)p)->code; }
    return 0;
}

static void cap_log(void *ctx, const char *line)
{
    cap_t *c = ctx;
    size_t used = strlen(c->log);
    snprintf(c->log + used, sizeof c->log - used, "%s\n", line);
}

typedef struct {
    uint8_t img[SLOT_SIZE];
    size_t  len;
} target_t;

static target_t tgt;   /* big: kept off the stack */

/* The shipped firmware with a few sectors patched, as release `version`. */
static int make_target(target_t *t, uint32_t version)
{
    uint8_t *bin = malloc(base_len);
    if (!bin) return -1;
    memcpy(bin, base_bin, base_len);
    for (size_t k = 1; k <= 3; k++) {
        size_t at = k * SECTOR_SIZE - DESC_LEN + 100;
        if (at + 16 < base_len) memset(bin + at, (int)(0x30 + k), 16);
    }
    int rc = image_build(bin, base_len, version, root_key, t->img, &t->len);
    free(bin);
    return rc;
}

static int open_dev(flash_t *f, cap_t *cap, const char *name)
{
    char path[1024];
    cb_err_t err;
    snprintf(path, sizeof path, "%s/%s.bin", workdir, name);
    remove(path);
    memset(cap, 0, sizeof *cap);
    return device_open_or_create(f, path, base_path, root_key, cap_log, cap, &err);
}

typedef struct {
    flash_t        *f;
    cap_t          *cap;
    device_session_t s;
    int             rc;
    manifest_t      man;
    plan_t          plan;
    uint8_t         table[SECTORS_SLOT * SECTOR_HASH_LEN];
    msg_plan_begin_t pb;
} drv_t;

static drv_t drv;

static int feed(drv_t *d, int type, const void *p, size_t n)
{
    return d->rc = device_session_feed(&d->s, type, p, n);
}

/* HELLO/CHALLENGE, leaving the session waiting for the server's offer. */
static int drv_open(drv_t *d, flash_t *f, cap_t *cap, const target_t *t)
{
    memset(d, 0, sizeof *d);
    d->f = f;
    d->cap = cap;
    if (device_session_start(&d->s, f, dev_key, root_key, cap_send, cap, cap_log, cap, NULL)
            != DEV_PENDING)
        return -1;
    msg_challenge_t ch;
    memset(&ch, 7, sizeof ch);
    if (feed(d, MSG_CHALLENGE, &ch, sizeof ch) != DEV_PENDING) return -1;

    manifest_build(f, &d->man);
    plan_build(&d->man, SLOT_A, SLOT_B, t->img, t->len, &d->plan);
    image_table(t->img, t->len, d->table);

    const image_desc_t *desc = image_desc(t->img);
    memset(&d->pb, 0, sizeof d->pb);
    d->pb.version    = desc->version;
    d->pb.step_count = (uint16_t)d->plan.count;
    d->pb.dest_slot  = SLOT_B;
    d->pb.image_len  = desc->image_len;
    d->pb.flags      = desc->flags;
    memcpy(d->pb.image_sha256, desc->sha256, SHA256_LEN);
    memcpy(d->pb.table_root, desc->table_root, SHA256_LEN);
    memcpy(d->pb.image_mac, desc->mac, SHA256_LEN);
    return 0;
}

static int send_begin(drv_t *d) { return feed(d, MSG_PLAN_BEGIN, &d->pb, sizeof d->pb); }

/* The table in order; flip_at >= 0 corrupts that byte of it on the wire. */
static int send_table(drv_t *d, size_t table_len, long flip_at)
{
    for (size_t off = 0; off < table_len; off += 256) {
        uint8_t buf[TABLE_HDR + 256];
        uint16_t len = (uint16_t)(table_len - off < 256 ? table_len - off : 256);
        uint16_t o16 = (uint16_t)off;
        memcpy(buf, &o16, 2);
        memcpy(buf + 2, &len, 2);
        memcpy(buf + TABLE_HDR, d->table + off, len);
        if (flip_at >= (long)off && flip_at < (long)(off + len)) buf[TABLE_HDR + flip_at - off] ^= 1;
        if (feed(d, MSG_PLAN_TABLE, buf, TABLE_HDR + len) != DEV_PENDING) return d->rc;
    }
    return feed(d, MSG_PLAN_TABLE_END, NULL, 0);
}

/* One step, as a server would send it. patch, if given, edits the decoded
 * sector before it goes out (the table still lists the original). */
static int send_step(drv_t *d, const target_t *t, const step_t *s,
                     void (*patch)(uint8_t sector[SECTOR_SIZE]))
{
    if (s->op == OP_COPY) {
        msg_step_copy_t c = { s->dst, s->src_slot, 0, s->src_index };
        return feed(d, MSG_STEP_COPY, &c, sizeof c);
    }
    uint8_t sect[SECTOR_SIZE];
    memcpy(sect, t->img + (size_t)s->dst * SECTOR_SIZE, SECTOR_SIZE);
    if (patch) patch(sect);
    for (size_t off = 0; off < SECTOR_SIZE; off += WRITE_CHUNK) {
        uint8_t buf[WRITE_HDR + WRITE_CHUNK];
        uint16_t o16 = (uint16_t)off, len = WRITE_CHUNK;
        memcpy(buf + 0, &s->dst, 2);
        memcpy(buf + 2, &o16, 2);
        memcpy(buf + 4, &len, 2);
        memcpy(buf + WRITE_HDR, sect + off, len);
        if (feed(d, MSG_STEP_WRITE, buf, WRITE_HDR + len) != DEV_PENDING) return d->rc;
    }
    msg_step_write_end_t we = { .dst = s->dst, .compress = COMPRESS_RAW, .enc_len = SECTOR_SIZE };
    return feed(d, MSG_STEP_WRITE_END, &we, sizeof we);
}

/* Every step in plan order, then PLAN_END and COMMIT. */
static int send_rest(drv_t *d, const target_t *t)
{
    for (int k = 0; k < d->plan.count; k++)
        if (send_step(d, t, &d->plan.step[k], NULL) != DEV_PENDING) return d->rc;
    if (feed(d, MSG_PLAN_END, NULL, 0) != DEV_PENDING) return d->rc;
    return feed(d, MSG_COMMIT, NULL, 0);
}

static int untouched(const flash_t *f)
{
    return f->n_erase == 0 && f->n_prog == 0;
}

/* A fresh device, a session through PLAN_BEGIN's checks, the target built. */
#define SETUP(name, version) \
    flash_t f; cap_t cap; \
    REQUIRE(open_dev(&f, &cap, name) == 0, "cannot create the device"); \
    REQUIRE(make_target(&tgt, version) == 0, "cannot build the target"); \
    REQUIRE(drv_open(&drv, &f, &cap, &tgt) == 0, "session did not start"); \
    f.n_erase = f.n_prog = 0; f.prog_bytes = 0

/* ------------------------------------------------------------------ cases */

static int t_honest(void)
{
    SETUP("honest", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) == DEV_PENDING,
            "table refused: %s", drv.s.error);
    REQUIRE(send_rest(&drv, &tgt) == DEV_STAGED, "plan not staged: %s", drv.s.error);
    REQUIRE(cap.naks == 0, "%d NAKs on an honest plan", cap.naks);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(strstr(cap.log, "trial boot"), "the honest plan did not reach a trial boot:\n%s", cap.log);
    flash_file_close(&f);
    return 0;
}

static int t_bad_signature(void)
{
    SETUP("bad_mac", 2);
    drv.pb.image_mac[0] ^= 1;
    REQUIRE(send_begin(&drv) == DEV_ERROR, "a bad release signature was accepted");
    REQUIRE(strstr(drv.s.error, "signature"), "wrong reason: %s", drv.s.error);
    REQUIRE(untouched(&f), "flash written before the signature checked out");
    flash_file_close(&f);
    return 0;
}

static int t_bad_table_root(void)
{
    /* table_root is inside the signed prefix, so swapping it breaks the mac. */
    SETUP("bad_root", 2);
    drv.pb.table_root[0] ^= 1;
    REQUIRE(send_begin(&drv) == DEV_ERROR, "a re-rooted plan was accepted");
    REQUIRE(strstr(drv.s.error, "signature"), "wrong reason: %s", drv.s.error);
    REQUIRE(untouched(&f), "flash written before the signature checked out");
    flash_file_close(&f);
    return 0;
}

static int t_table_mismatch(void)
{
    /* A genuine signature, a table that is not the one it covers. */
    SETUP("bad_table", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, 5) == DEV_ERROR,
            "a table that does not fold to table_root was accepted");
    REQUIRE(strstr(drv.s.error, "table_root"), "wrong reason: %s", drv.s.error);
    REQUIRE(untouched(&f), "flash written before the table was authenticated");
    REQUIRE(send_step(&drv, &tgt, &drv.plan.step[0], NULL) == DEV_ERROR, "a step followed a refused table");
    REQUIRE(untouched(&f), "a step reached flash after the table was refused");
    flash_file_close(&f);
    return 0;
}

static int t_step_before_table(void)
{
    SETUP("early_step", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_step(&drv, &tgt, &drv.plan.step[0], NULL) == DEV_ERROR,
            "a step was accepted before the table");
    REQUIRE(untouched(&f), "a step reached flash before the table was authenticated");
    flash_file_close(&f);
    return 0;
}

static void corrupt_sector(uint8_t sector[SECTOR_SIZE]) { sector[SECTOR_SIZE - 1] ^= 0xFF; }

static int t_step_not_in_table(void)
{
    /* The sector is internally consistent (a server could hash it any way it
     * likes) but is not what the signed table lists for that sector. */
    SETUP("off_table", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) == DEV_PENDING,
            "table refused: %s", drv.s.error);

    int k = 0;
    while (k < drv.plan.count && drv.plan.step[k].op != OP_WRITE) k++;
    REQUIRE(k < drv.plan.count, "the plan has no WRITE step");
    const step_t *s = &drv.plan.step[k];

    uint8_t before[SECTOR_SIZE], after[SECTOR_SIZE];
    flash_read(&f, slot_base(SLOT_B) + (size_t)s->dst * SECTOR_SIZE, before, SECTOR_SIZE);
    f.n_erase = f.n_prog = 0;
    REQUIRE(send_step(&drv, &tgt, s, corrupt_sector) == DEV_PENDING, "session died: %s", drv.s.error);
    REQUIRE(cap.naks == 1 && cap.last_nak == NAK_HASH_MISMATCH, "expected one HASH_MISMATCH NAK");
    flash_read(&f, slot_base(SLOT_B) + (size_t)s->dst * SECTOR_SIZE, after, SECTOR_SIZE);
    REQUIRE(!memcmp(before, after, SECTOR_SIZE) && untouched(&f), "the off-table sector reached flash");
    flash_file_close(&f);
    return 0;
}

static int t_descriptor_not_last(void)
{
    SETUP("desc_first", 2);
    REQUIRE(drv.plan.count >= 2, "the plan has no second step");
    REQUIRE(drv.plan.step[drv.plan.count - 1].dst == DESC_SECTOR, "planner did not put the descriptor last");
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) == DEV_PENDING,
            "table refused: %s", drv.s.error);
    REQUIRE(send_step(&drv, &tgt, &drv.plan.step[drv.plan.count - 1], NULL) == DEV_PENDING,
            "the descriptor sector was refused: %s", drv.s.error);
    REQUIRE(send_step(&drv, &tgt, &drv.plan.step[0], NULL) == DEV_ERROR,
            "a step was accepted after the descriptor sector");
    REQUIRE(strstr(drv.s.error, "descriptor"), "wrong reason: %s", drv.s.error);
    flash_file_close(&f);
    return 0;
}

static void edit_descriptor(uint8_t sector[SECTOR_SIZE])
{
    /* Inside the hole: the table cannot see it, PLAN_END must. */
    sector[DESC_OFF % SECTOR_SIZE + offsetof(image_desc_t, reserved)] ^= 0x55;
}

static int t_descriptor_swapped(void)
{
    SETUP("desc_swap", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) == DEV_PENDING,
            "table refused: %s", drv.s.error);
    for (int k = 0; k < drv.plan.count; k++) {
        const step_t *s = &drv.plan.step[k];
        REQUIRE(send_step(&drv, &tgt, s, s->dst == DESC_SECTOR ? edit_descriptor : NULL) == DEV_PENDING,
                "step %d refused: %s", k, drv.s.error);
    }
    REQUIRE(cap.naks == 0, "the table should not see a descriptor edit");
    REQUIRE(feed(&drv, MSG_PLAN_END, NULL, 0) == DEV_ERROR, "a swapped descriptor was staged");
    REQUIRE(strstr(drv.s.error, "descriptor"), "wrong reason: %s", drv.s.error);

    meta_t m;
    meta_load(&f, &m);
    REQUIRE(m.plan_state != PLAN_STAGING, "the plan was persisted");
    flash_file_close(&f);
    return 0;
}

static int t_plan_begin_bounds(void)
{
    SETUP("bounds", 2);
    drv.pb.image_len += 1;
    REQUIRE(send_begin(&drv) == DEV_ERROR, "an image_len that is not a sector multiple was accepted");
    REQUIRE(untouched(&f), "flash written for a bad PLAN_BEGIN");
    flash_file_close(&f);

    /* Version 0 against the shipped version 1: signed, but a downgrade. */
    REQUIRE(make_target(&tgt, 0) == 0, "cannot build the target");
    REQUIRE(open_dev(&f, &cap, "downgrade") == 0, "cannot create the device");
    REQUIRE(drv_open(&drv, &f, &cap, &tgt) == 0, "session did not start");
    REQUIRE(send_begin(&drv) == DEV_ERROR, "a downgrade was accepted");
    REQUIRE(strstr(drv.s.error, "downgrade"), "wrong reason: %s", drv.s.error);
    flash_file_close(&f);
    return 0;
}

/* An armed plan to attack: a full honest session, stopped before any boot. */
static int stage_armed(flash_t *f, cap_t *cap, const char *name)
{
    if (open_dev(f, cap, name) < 0) return -1;
    if (make_target(&tgt, 2) < 0 || drv_open(&drv, f, cap, &tgt) < 0) return -1;
    if (send_begin(&drv) != DEV_PENDING) return -1;
    if (send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) != DEV_PENDING) return -1;
    return send_rest(&drv, &tgt) == DEV_STAGED ? 0 : -1;
}

static int t_meta_tampered(void)
{
    flash_t f;
    cap_t cap;
    char path[1024];
    REQUIRE(stage_armed(&f, &cap, "meta_tamper") == 0, "cannot stage a plan");
    meta_t armed;
    meta_load(&f, &armed);
    REQUIRE(armed.plan_state == PLAN_STAGING, "the plan is not on the books");
    flash_file_close(&f);

    /* Edit the live bank in the file, no key, no code path: a bank with the
     * old CRC-32 scheme would only have needed its checksum redone. */
    snprintf(path, sizeof path, "%s/meta_tamper.bin", workdir);
    size_t len;
    uint8_t *img = read_file(path, &len);
    REQUIRE(img, "cannot read the flash file");
    int live = -1;
    uint32_t gen = 0;
    for (int b = 0; b < 2; b++) {
        meta_t m;
        memcpy(&m, img + (size_t)(b ? META1_UNIT : META0_UNIT) * ERASE_SIZE, sizeof m);
        if (m.magic == META_MAGIC && m.generation >= gen) { gen = m.generation; live = b; }
    }
    REQUIRE(live >= 0 && gen == armed.generation, "cannot find the live bank");
    img[(size_t)(live ? META1_UNIT : META0_UNIT) * ERASE_SIZE + offsetof(meta_t, attempts_left)] ^= 3;
    REQUIRE(write_file(path, img, len) == 0, "cannot write the flash file");
    free(img);

    REQUIRE(flash_file_open(&f, path) == 0, "cannot reopen");
    meta_t after;
    meta_load(&f, &after);
    REQUIRE(after.generation < armed.generation, "the tampered bank was believed (generation %u)",
            after.generation);
    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "applying plan"), "the tampered plan was applied:\n%s", cap.log);
    flash_file_close(&f);
    return 0;
}

static int t_forged_plan_table(void)
{
    /* The attacker holds dev_key (so the meta mac is genuine) but not the
     * release key: the bootloader must still refuse a plan whose table is
     * not the one the signed descriptor names. */
    flash_t f;
    cap_t cap;
    REQUIRE(stage_armed(&f, &cap, "forged_table") == 0, "cannot stage a plan");

    meta_t m;
    meta_load(&f, &m);
    m.target_hash[1][0] ^= 1;
    REQUIRE(meta_store(&f, &m) == 0, "cannot store the forged plan: %s", f.err.msg);
    REQUIRE(journal_set(&f, JRN_ARM_INDEX, 0x0000) == 0, "cannot re-arm the plan: %s", f.err.msg);

    uint8_t before[SLOT_SIZE], after[SLOT_SIZE];
    flash_read(&f, slot_base(SLOT_B), before, SLOT_SIZE);
    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(strstr(cap.log, "signed table_root"), "the forged plan was not refused for its table:\n%s", cap.log);
    REQUIRE(!strstr(cap.log, "trial boot"), "the forged plan reached a trial boot");

    flash_read(&f, slot_base(SLOT_B), after, SLOT_SIZE);
    REQUIRE(!memcmp(before + SECTOR_SIZE, after + SECTOR_SIZE, SLOT_SIZE - SECTOR_SIZE),
            "the bootloader touched the pending slot beyond the unroll");
    meta_load(&f, &m);
    REQUIRE(m.plan_state == PLAN_NONE && m.active_slot == SLOT_A, "plan_state %d active %d after the refusal",
            m.plan_state, m.active_slot);
    flash_file_close(&f);
    return 0;
}

static int t_forged_downgrade(void)
{
    /* After an honest v2 update slot A still holds the genuinely signed v1.
     * A dev_key holder forges a plan that points back at it. */
    flash_t f;
    cap_t cap;
    REQUIRE(stage_armed(&f, &cap, "forged_downgrade") == 0, "cannot stage a plan");
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(device_confirm(&f, cap_log, &cap) == 0, "confirm: %s", f.err.msg);

    static uint8_t old[SLOT_SIZE];
    flash_read(&f, slot_base(SLOT_A), old, SLOT_SIZE);
    const image_desc_t *d = image_desc(old);
    REQUIRE(d->magic == IMG_MAGIC && d->version == 1, "slot A does not hold v1");

    meta_t m;
    meta_load(&f, &m);
    REQUIRE(m.active_slot == SLOT_B, "the v2 update did not land");
    m.plan_state     = PLAN_STAGING;
    m.pending_slot   = SLOT_A;
    m.target_version = d->version;
    m.image_len      = d->image_len;
    memcpy(m.target_sha256, d->sha256, SHA256_LEN);
    memcpy(m.target_mac, d->mac, SHA256_LEN);
    image_table(old, d->image_len, (uint8_t *)m.target_hash);
    memset(m.op, OP_NONE, sizeof m.op);
    REQUIRE(meta_store(&f, &m) == 0, "cannot store the forged plan: %s", f.err.msg);
    REQUIRE(journal_set(&f, JRN_ARM_INDEX, 0x0000) == 0, "cannot arm: %s", f.err.msg);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(strstr(cap.log, "downgrade"), "the forged downgrade was not refused:\n%s", cap.log);
    REQUIRE(!strstr(cap.log, "trial boot"), "the forged downgrade reached a trial boot");
    meta_load(&f, &m);
    REQUIRE(m.active_slot == SLOT_B && m.plan_state == PLAN_NONE, "active %d plan_state %d",
            m.active_slot, m.plan_state);
    flash_file_close(&f);
    return 0;
}

static int t_revert_active(void)
{
    /* A forged trial plan whose pending slot is the running one must not
     * revoke the running image. */
    flash_t f;
    cap_t cap;
    REQUIRE(open_dev(&f, &cap, "revert_active") == 0, "cannot create the device");
    meta_t m;
    meta_load(&f, &m);
    m.plan_state    = PLAN_TRIAL;
    m.pending_slot  = m.active_slot;
    m.attempts_left = 0;
    REQUIRE(meta_store(&f, &m) == 0, "cannot store the forged plan: %s", f.err.msg);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "revoked"), "the running image was revoked:\n%s", cap.log);
    REQUIRE(!strstr(cap.log, "does not verify"), "the active slot no longer verifies:\n%s", cap.log);
    meta_load(&f, &m);
    REQUIRE(m.plan_state == PLAN_NONE && m.pending_slot == SLOT_NONE, "plan_state %d pending %d",
            m.plan_state, m.pending_slot);
    flash_file_close(&f);
    return 0;
}

static int forge_trial(flash_t *f, slot_id_t pending)
{
    meta_t m;
    meta_load(f, &m);
    m.plan_state    = PLAN_TRIAL;
    m.pending_slot  = pending;
    m.attempts_left = MAX_TRIES;
    return meta_store(f, &m);
}

static int t_forged_trial(void)
{
    /* A meta forged straight into TRIAL skips the STAGING->TRIAL verify:
     * every trial boot must check the pending slot itself. */
    flash_t f;
    cap_t cap;
    meta_t m;

    /* Unsigned bytes: a fresh device's slot B holds no image. */
    REQUIRE(open_dev(&f, &cap, "forged_trial_empty") == 0, "cannot create the device");
    REQUIRE(forge_trial(&f, SLOT_B) == 0, "cannot store the forged plan: %s", f.err.msg);
    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "trial boot"), "an unverified slot was trial-booted:\n%s", cap.log);
    meta_load(&f, &m);
    REQUIRE(m.active_slot == SLOT_A && m.plan_state == PLAN_NONE, "active %d plan_state %d",
            m.active_slot, m.plan_state);
    flash_file_close(&f);

    /* An older signed image: after an honest v2 update slot A holds v1. */
    REQUIRE(stage_armed(&f, &cap, "forged_trial_old") == 0, "cannot stage a plan");
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(device_confirm(&f, cap_log, &cap) == 0, "confirm: %s", f.err.msg);
    meta_load(&f, &m);
    m.target_version = 1;
    image_desc_t d;
    image_desc_read(&f, SLOT_A, &d);
    m.image_len = d.image_len;
    memcpy(m.target_sha256, d.sha256, SHA256_LEN);
    REQUIRE(meta_store(&f, &m) == 0, "cannot store: %s", f.err.msg);
    REQUIRE(forge_trial(&f, SLOT_A) == 0, "cannot store the forged plan: %s", f.err.msg);
    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, root_key, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(strstr(cap.log, "downgrade"), "the forged rollback was not refused:\n%s", cap.log);
    REQUIRE(!strstr(cap.log, "trial boot"), "the older image was trial-booted");
    meta_load(&f, &m);
    REQUIRE(m.active_slot == SLOT_B && m.plan_state == PLAN_NONE, "active %d plan_state %d",
            m.active_slot, m.plan_state);
    flash_file_close(&f);
    return 0;
}

static void flip_vector(uint8_t sector[SECTOR_SIZE]) { sector[0] ^= 1; }

static int t_descriptor_resend(void)
{
    /* A NAKed descriptor sector does not close the plan: the resend lands. */
    SETUP("desc_resend", 2);
    REQUIRE(send_begin(&drv) == DEV_PENDING, "PLAN_BEGIN refused: %s", drv.s.error);
    REQUIRE(send_table(&drv, (size_t)(tgt.len / SECTOR_SIZE) * SECTOR_HASH_LEN, -1) == DEV_PENDING,
            "table refused: %s", drv.s.error);
    for (int k = 0; k < drv.plan.count; k++) {
        const step_t *s = &drv.plan.step[k];
        if (s->dst == DESC_SECTOR) {
            REQUIRE(send_step(&drv, &tgt, s, flip_vector) == DEV_PENDING, "bad descriptor: %s", drv.s.error);
            REQUIRE(cap.naks == 1, "a corrupt descriptor sector was not NAKed");
        }
        REQUIRE(send_step(&drv, &tgt, s, NULL) == DEV_PENDING, "step %d refused: %s", k, drv.s.error);
    }
    REQUIRE(feed(&drv, MSG_PLAN_END, NULL, 0) == DEV_PENDING, "PLAN_END refused: %s", drv.s.error);
    REQUIRE(feed(&drv, MSG_COMMIT, NULL, 0) == DEV_STAGED, "COMMIT refused: %s", drv.s.error);
    flash_file_close(&f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: auth_test WORKDIR BASE.bin\n"); return 2; }
    workdir = argv[1];
    base_path = argv[2];
    poc_dev_key(dev_key);
    poc_root_key(root_key);
    base_bin = read_file(base_path, &base_len);
    if (!base_bin) { fprintf(stderr, "cannot read %s\n", base_path); return 2; }

    static int (*const cases[])(void) = {
        t_honest, t_bad_signature, t_bad_table_root, t_table_mismatch, t_step_before_table,
        t_step_not_in_table, t_descriptor_not_last, t_descriptor_swapped, t_plan_begin_bounds,
        t_meta_tampered, t_forged_plan_table, t_forged_downgrade, t_revert_active,
        t_descriptor_resend, t_forged_trial,
    };
    int bad = 0, n = (int)(sizeof cases / sizeof *cases);
    for (int i = 0; i < n; i++)
        if (cases[i]() < 0) bad++;
    free(base_bin);
    if (bad) { printf("FAIL  auth_test (%d of %d)\n", bad, n); return 1; }
    printf("PASS  auth_test (%d cases)\n", n);
    return 0;
}
