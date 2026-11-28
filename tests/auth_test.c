/* Plan authentication and meta integrity (SPEC.md; MIGRATION_FORMAT2.md); drives lib/device.c and lib/bootloader.c directly, message by message, to forge what a real server never would (bad descriptor seal/scheme, step order, floor violations, forged trial state, a rewritten meta bank).
 *
 *   auth_test WORKDIR BASE.bin
 *
 * WORKDIR gets one flash file per case. BASE.bin is the firmware every
 * device ships with (tests/common.sh's gen_random_bin).
 *
 * A reduced port of the pre-format2 auth_test.c: 5 of its 16 forged-message scenarios have format2 equivalents here; the rest no longer apply (no PLAN_TABLE round trip, no per-step ACK/NAK, no descriptor-as-a-step ordering). */
#include "bootloader.h"
#include "device.h"
#include "flash_file.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_image.h"
#include "format2_plan.h"
#include "host.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    (void)n;
    if (type == MSG2_STEP_ACK || type == MSG2_STEP_NAK) {
        msg2_step_ack_t a;
        memcpy(&a, p, sizeof a);
        if (type == MSG2_STEP_ACK) c->acks++;
        else { c->naks++; c->last_nak = a.code; }
    }
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

/* Kept off the stack (SLOT_SIZE-sized) and reused one case at a time, since main() runs every case_*() sequentially. */
static target_t         tgt;
static uint8_t           g_scratch[SLOT_SIZE];   /* format2_plan_build_from_active_slot()'s active_scratch */
static uint8_t           g_planbuf[DEVICE_PLAN_BUF_CAP];

static int open_dev(flash_t *f, cap_t *cap, const char *name)
{
    char path[1024];
    cb_err_t err;
    snprintf(path, sizeof path, "%s/%s.bin", workdir, name);
    remove(path);
    memset(cap, 0, sizeof *cap);
    return device_open_or_create(f, path, base_path, cap_log, cap, &err);
}

/* The shipped firmware as release `version`/`security_counter`; no key needed (format2 scheme 2 is unsigned). */
static int build_target(target_t *t, uint32_t version, uint32_t security_counter)
{
    return format2_image_build(base_bin, base_len, version, security_counter, t->img, &t->len);
}

static int build_plan(flash_t *f, int active_slot, const uint8_t *target_img,
                      uint8_t *out, size_t out_cap, size_t *out_len)
{
    return format2_plan_build_from_active_slot(f, active_slot, target_img, g_scratch, out, out_cap, out_len);
}

/* Fragments buf as MSG2_PLAN+MSG2_PLAN_END, same shape as server_session.c's send_fragmented(), straight into device_session_feed() since this test plays the server's role. Returns the final feed's result (DEV_PENDING or DEV_ERROR). */
static int send_plan(device_session_t *s, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    const size_t max_body = FRAME_MAX_PAYLOAD - PLAN2_FRAG_HDR;
    int rc = DEV_PENDING;

    while (off < len) {
        uint8_t frag[FRAME_MAX_PAYLOAD];
        uint16_t o16 = (uint16_t)off;
        uint16_t flen = (uint16_t)(len - off < max_body ? len - off : max_body);
        memcpy(frag, &o16, 2);
        memcpy(frag + 2, &flen, 2);
        memcpy(frag + PLAN2_FRAG_HDR, buf + off, flen);
        rc = device_session_feed(s, MSG2_PLAN, frag, PLAN2_FRAG_HDR + flen);
        if (rc != DEV_PENDING) return rc;
        off += flen;
    }
    return device_session_feed(s, MSG2_PLAN_END, NULL, 0);
}

/* Opens a fresh device, starts a session, builds an honest target/plan against the device's own active-slot bytes, and sends it. Returns send_plan()'s result; caller still owns f/cap/s and decides whether to send MSG2_COMMIT. */
static int stage_plan(flash_t *f, cap_t *cap, device_session_t *s, const char *name,
                      uint32_t version, uint32_t security_counter)
{
    meta_t m;
    size_t plan_len;

    if (open_dev(f, cap, name) < 0) return DEV_ERROR;
    if (device_session_start(s, f, cap_send, cap, cap_log, cap, NULL) != DEV_PENDING) return DEV_ERROR;
    if (build_target(&tgt, version, security_counter) < 0) return DEV_ERROR;
    meta_load(f, &m);
    if (build_plan(f, m.active_slot, tgt.img, g_planbuf, sizeof g_planbuf, &plan_len) < 0) return DEV_ERROR;
    return send_plan(s, g_planbuf, plan_len);
}

/* stage_plan() through to DEV_STAGED, armed with MSG2_COMMIT: a full honest session stopped right before any boot. */
static int stage_armed(flash_t *f, cap_t *cap, device_session_t *s, const char *name,
                       uint32_t version, uint32_t security_counter)
{
    if (stage_plan(f, cap, s, name, version, security_counter) != DEV_PENDING) return -1;
    return device_session_feed(s, MSG2_COMMIT, NULL, 0) == DEV_STAGED ? 0 : -1;
}

static int untouched(const flash_t *f)
{
    return f->n_erase == 0 && f->n_prog == 0;
}

/* ------------------------------------------------------------------ cases */

static int t2_honest(void)
{
    flash_t f;
    cap_t cap;
    device_session_t s;
    meta_t m;
    int pending;

    REQUIRE(stage_armed(&f, &cap, &s, "t2_honest", 2, 2) == 0, "cannot stage an honest plan: %s", s.error);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(strstr(cap.log, "trial boot"), "the honest plan did not reach a trial boot:\n%s", cap.log);
    meta_load(&f, &m);
    pending = m.pending_slot;
    REQUIRE(pending == SLOT_B, "pending slot %d, expected B", pending);

    REQUIRE(device_confirm(&f, cap_log, &cap) == 0, "confirm: %s", f.err.msg);
    meta_load(&f, &m);
    REQUIRE(m.active_slot == pending, "active_slot was not promoted to the confirmed (trial) slot");
    REQUIRE(m.plan_state == PLAN_COMMITTED, "plan_state %d after confirm, expected COMMITTED", m.plan_state);

    flash_file_close(&f);
    return 0;
}

static int t2_bad_seal(void)
{
    flash_t f;
    cap_t cap;
    device_session_t s;
    meta_t m;
    size_t plan_len;
    int rc;

    REQUIRE(open_dev(&f, &cap, "t2_bad_seal") == 0, "cannot create the device");
    REQUIRE(device_session_start(&s, &f, cap_send, &cap, cap_log, &cap, NULL) == DEV_PENDING,
            "session did not start");
    REQUIRE(build_target(&tgt, 2, 2) == 0, "cannot build the target");
    meta_load(&f, &m);
    REQUIRE(build_plan(&f, m.active_slot, tgt.img, g_planbuf, sizeof g_planbuf, &plan_len) == 0,
            "cannot build the plan");

    /* Flips image_sha256's first byte inside the sealed body (SPEC.md 4.1, not the scheme byte), caught only at the seal check (stage 3), never an earlier refusal. */
    g_planbuf[CHUNKY_PLAN_HEADER_DESCRIPTOR_OFFSET + CHUNKY_DESCRIPTOR_BODY_IMAGE_SHA256_OFFSET] ^= 0x01;

    REQUIRE(untouched(&f), "flash written before the plan was even sent");
    rc = send_plan(&s, g_planbuf, plan_len);
    REQUIRE(rc == DEV_ERROR, "a plan with a tampered, uncomputed-seal descriptor body was accepted");
    REQUIRE(strstr(s.error, format2_err_str(FORMAT2_ESIGNATURE)), "wrong reason: %s", s.error);
    REQUIRE(untouched(&f), "flash written before the seal checked out");

    flash_file_close(&f);
    return 0;
}

/* Shared by t2_bad_scheme()'s two sub-cases. */
static int one_bad_scheme(const char *name, uint8_t scheme, format2_err_t want)
{
    flash_t f;
    cap_t cap;
    device_session_t s;
    meta_t m;
    size_t plan_len;
    int rc;

    REQUIRE(open_dev(&f, &cap, name) == 0, "cannot create the device");
    REQUIRE(device_session_start(&s, &f, cap_send, &cap, cap_log, &cap, NULL) == DEV_PENDING,
            "session did not start");
    REQUIRE(build_target(&tgt, 2, 2) == 0, "cannot build the target");
    meta_load(&f, &m);
    REQUIRE(build_plan(&f, m.active_slot, tgt.img, g_planbuf, sizeof g_planbuf, &plan_len) == 0,
            "cannot build the plan");

    g_planbuf[CHUNKY_PLAN_HEADER_DESCRIPTOR_OFFSET + CHUNKY_DESCRIPTOR_BODY_SCHEME_OFFSET] = scheme;

    REQUIRE(untouched(&f), "flash written before the plan was even sent");
    rc = send_plan(&s, g_planbuf, plan_len);
    REQUIRE(rc == DEV_ERROR, "scheme byte %u was accepted", (unsigned)scheme);
    REQUIRE(strstr(s.error, format2_err_str(want)), "wrong reason for scheme %u: %s", (unsigned)scheme, s.error);
    REQUIRE(untouched(&f), "flash written for scheme %u before the seal was even checked", (unsigned)scheme);

    flash_file_close(&f);
    return 0;
}

static int t2_bad_scheme(void)
{
    /* Ed25519 (SPEC.md 4.2's scheme 1): known, well-formed, but this device is unsigned (scheme 2); refused as unsupported before the seal is even computed. */
    if (one_bad_scheme("t2_bad_scheme_ed25519", CHUNKY_SCHEME_ED25519, FORMAT2_ESCHEME_UNSUPPORTED) < 0)
        return -1;
    /* A value naming neither scheme SPEC.md defines: refused even earlier, at the stage-1 raw-byte check, before any seal work. */
    if (one_bad_scheme("t2_bad_scheme_unknown", 99, FORMAT2_ESCHEME) < 0)
        return -1;
    return 0;
}

typedef struct { size_t off, len; uint8_t op; uint16_t idx; } step_info_t;

/* Walks a plan body into step_info_t records (same framing as format2_plan_walk(), SPEC.md 5.3), without checking hashes/order/ranges: this test wants step positions, not a verdict. -1 on a malformed step or out of room, else the count. */
static int parse_steps(const uint8_t *body, size_t body_len, step_info_t *out, int max)
{
    size_t at = 0;
    int n = 0;

    while (at < body_len) {
        chunky_step_view_t v;
        if (n >= max || chunky_step_view(body + at, body_len - at, &v) != 0) return -1;
        out[n].off = at;
        out[n].len = v.len;
        out[n].op = chunky_step_tag(&v);
        if (out[n].op == CHUNKY_STEP_ERASE) {
            const chunky_step_erase_view_t *e;
            if (chunky_step_as_erase(&v, &e) != 0) return -1;
            out[n].idx = chunky_step_erase_block(e);
        } else if (out[n].op == CHUNKY_STEP_COPY) {
            const chunky_step_copy_view_t *c;
            if (chunky_step_as_copy(&v, &c) != 0) return -1;
            out[n].idx = chunky_step_copy_dst(c);
        } else if (out[n].op == CHUNKY_STEP_WRITE) {
            const chunky_step_write_view_t *w;
            if (chunky_step_as_write(&v, &w) != 0) return -1;
            out[n].idx = chunky_step_write_dst(w);
        } else {
            return -1;
        }
        at += v.len;
        n++;
    }
    return n;
}

static int t2_step_order(void)
{
    flash_t f;
    cap_t cap;
    device_session_t s;
    meta_t m;
    size_t plan_len;
    static step_info_t steps[SECTORS_SLOT * 2 + 4];
    int n, j = -1, earlier_chunk = -1, rc;
    uint8_t flashbuf[FORMAT2_CHUNK_LEN];

    REQUIRE(open_dev(&f, &cap, "t2_step_order") == 0, "cannot create the device");
    REQUIRE(device_session_start(&s, &f, cap_send, &cap, cap_log, &cap, NULL) == DEV_PENDING,
            "session did not start");
    REQUIRE(build_target(&tgt, 2, 2) == 0, "cannot build the target");
    meta_load(&f, &m);
    REQUIRE(build_plan(&f, m.active_slot, tgt.img, g_planbuf, sizeof g_planbuf, &plan_len) == 0,
            "cannot build the plan");

    n = parse_steps(g_planbuf + CHUNKY_PLAN_HEADER_LEN, plan_len - CHUNKY_PLAN_HEADER_LEN,
                    steps, (int)(sizeof steps / sizeof steps[0]));
    REQUIRE(n > 0, "cannot parse the plan's own steps");

    /* A data step for chunk >= 2, directly followed by the next block's ERASE: the structure format2_plan_build() always produces when chunks_per_block == 1, so this pair exists whenever the image spans >= 3 non-blank chunks. */
    for (int k = 0; k + 1 < n; k++) {
        if (steps[k].op != CHUNKY_STEP_ERASE && steps[k].idx >= 2 &&
            steps[k + 1].op == CHUNKY_STEP_ERASE) {
            j = k;
            break;
        }
    }
    REQUIRE(j >= 0, "no data step past the first block has a following ERASE to swap with");

    /* Swaps the two steps' byte ranges in place (same total length); SPEC.md 5.4's strictly-increasing position check must then fail on the data step, now out of order. */
    {
        uint8_t tmp[13 + FORMAT2_CHUNK_LEN];
        size_t base = CHUNKY_PLAN_HEADER_LEN + steps[j].off;
        size_t len_j = steps[j].len, len_j1 = steps[j + 1].len;
        REQUIRE(len_j + len_j1 <= sizeof tmp, "swap buffer too small");
        memcpy(tmp, g_planbuf + base + len_j, len_j1);
        memcpy(tmp + len_j1, g_planbuf + base, len_j);
        memcpy(g_planbuf + base, tmp, len_j + len_j1);
    }

    /* The nearest earlier data step (strictly before the swapped pair, so unaffected) must have already landed, verifiable, by the time the walk stops at the violation. */
    for (int k = j - 1; k >= 0; k--) {
        if (steps[k].op != CHUNKY_STEP_ERASE) { earlier_chunk = steps[k].idx; break; }
    }
    REQUIRE(earlier_chunk >= 0, "no earlier data step to verify survived the violation");

    rc = send_plan(&s, g_planbuf, plan_len);
    REQUIRE(rc == DEV_ERROR, "a plan with two swapped adjacent steps was accepted");
    REQUIRE(strstr(s.error, format2_err_str(FORMAT2_EORDER)), "wrong reason: %s", s.error);

    flash_read(&f, slot_base(SLOT_B) + (size_t)earlier_chunk * FORMAT2_CHUNK_LEN, flashbuf, FORMAT2_CHUNK_LEN);
    REQUIRE(!memcmp(flashbuf, tgt.img + (size_t)earlier_chunk * FORMAT2_CHUNK_LEN, FORMAT2_CHUNK_LEN),
            "chunk %d, applied before the swapped pair, is not actually in flash", earlier_chunk);

    flash_file_close(&f);
    return 0;
}

static int t2_rollback_floor(void)
{
    flash_t f, f2;
    cap_t cap, cap2;
    device_session_t s;
    meta_t m, before, after, forged;
    size_t plan_len;
    long e0, p0;
    int rc, active_before, old_slot;

    /* (a) session-level: an honest update+confirm raises the floor to its security_counter (SPEC.md 12.4); a later plan sealed below that floor is refused before any write. */
    REQUIRE(stage_armed(&f, &cap, &s, "t2_rollback_a", 5, 5) == 0, "cannot stage the floor-raising plan");
    REQUIRE(bootloader_boot(&f, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(device_confirm(&f, cap_log, &cap) == 0, "confirm: %s", f.err.msg);
    meta_load(&f, &m);
    REQUIRE(m.security_floor == 5, "security_floor is %u, expected 5 after confirming security_counter 5",
            m.security_floor);

    e0 = f.n_erase;
    p0 = f.n_prog;
    REQUIRE(device_session_start(&s, &f, cap_send, &cap, cap_log, &cap, NULL) == DEV_PENDING,
            "second session did not start");
    REQUIRE(build_target(&tgt, 6, 3) == 0, "cannot build the below-floor target");
    REQUIRE(build_plan(&f, m.active_slot, tgt.img, g_planbuf, sizeof g_planbuf, &plan_len) == 0,
            "cannot build the plan");
    rc = send_plan(&s, g_planbuf, plan_len);
    REQUIRE(rc == DEV_ERROR, "a plan with security_counter 3 under floor 5 was accepted");
    REQUIRE(strstr(s.error, format2_err_str(FORMAT2_EROLLBACK)), "wrong reason: %s", s.error);
    REQUIRE(f.n_erase == e0 && f.n_prog == p0, "flash written for a below-floor plan");
    flash_file_close(&f);

    /* (b) boot-level: same gate, reached from plan_check() at STAGING->TRIAL instead of format2_plan_begin(): meta_t forged straight into PLAN_STAGING, naming an older, genuinely-sealed image as pending after a real update already raised the floor. */
    REQUIRE(stage_armed(&f2, &cap2, &s, "t2_rollback_b", 7, 7) == 0, "cannot stage the floor-raising plan");
    REQUIRE(bootloader_boot(&f2, cap_log, &cap2, NULL) == 0, "boot: %s", f2.err.msg);
    REQUIRE(device_confirm(&f2, cap_log, &cap2) == 0, "confirm: %s", f2.err.msg);
    meta_load(&f2, &before);
    REQUIRE(before.security_floor == 7, "security_floor is %u, expected 7", before.security_floor);
    active_before = before.active_slot;        /* slot B, the just-confirmed v7 image */
    old_slot = OTHER_SLOT(active_before);       /* slot A, still the factory v1 image, sec_counter 1 */

    forged = before;
    forged.plan_state     = PLAN_STAGING;
    forged.pending_slot    = (uint8_t)old_slot;
    forged.target_version  = 1;
    forged.image_len       = (uint32_t)FORMAT2_IMAGE_LEN;
    forged.step_count      = 0;
    REQUIRE(meta_store(&f2, &forged) == 0, "cannot store the forged plan: %s", f2.err.msg);

    cap2.log[0] = 0;
    REQUIRE(bootloader_boot(&f2, cap_log, &cap2, NULL) == 0, "boot: %s", f2.err.msg);
    REQUIRE(!strstr(cap2.log, "trial boot"), "a rolled-back image reached a trial boot:\n%s", cap2.log);
    meta_load(&f2, &after);
    /* The security claim: the device never moved onto the rolled-back image. plan_check()'s refusal unrolls the forged plan to PLAN_NONE (DESIGN.md 8), a real state change, but the safe terminal one, never TRIAL. */
    REQUIRE(after.active_slot == active_before, "active_slot moved from %d to %d on a refused rollback",
            active_before, after.active_slot);
    REQUIRE(after.plan_state == PLAN_NONE,
            "plan_state %d after the refusal, expected NONE (unrolled cleanly, never TRIAL)", after.plan_state);
    flash_file_close(&f2);
    return 0;
}

static int t2_forged_trial(void)
{
    flash_t f;
    cap_t cap;
    meta_t m;
    int active;
    uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
    chunky_descriptor_body_view_t body;

    /* (a) pending_slot names a slot with no valid image (fresh device's blank slot B). A meta forged into TRIAL skips the STAGING->TRIAL verify, so every trial boot must check the pending slot unconditionally. */
    REQUIRE(open_dev(&f, &cap, "t2_forged_trial_blank") == 0, "cannot create the device");
    meta_load(&f, &m);
    m.plan_state    = PLAN_TRIAL;
    m.pending_slot  = (uint8_t)OTHER_SLOT(m.active_slot);
    m.attempts_left = MAX_TRIES;
    REQUIRE(meta_store(&f, &m) == 0, "cannot store the forged plan: %s", f.err.msg);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "trial boot"), "an unverified slot was trial-booted:\n%s", cap.log);
    meta_load(&f, &m);
    REQUIRE(m.plan_state == PLAN_NONE, "plan_state %d, expected NONE (cleanly unarmed/reverted)", m.plan_state);
    flash_file_close(&f);

    /* (b) pending_slot == active_slot: the bootloader must not treat or revoke the running image as untrusted just because it's also (falsely) the named pending slot. */
    REQUIRE(open_dev(&f, &cap, "t2_forged_trial_self") == 0, "cannot create the device");
    meta_load(&f, &m);
    active = m.active_slot;
    m.plan_state    = PLAN_TRIAL;
    m.pending_slot  = (uint8_t)active;
    m.attempts_left = 0;
    REQUIRE(meta_store(&f, &m) == 0, "cannot store the forged plan: %s", f.err.msg);

    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "revoked"), "the running image was revoked:\n%s", cap.log);
    REQUIRE(!strstr(cap.log, "does not verify"), "the active slot no longer verifies:\n%s", cap.log);
    meta_load(&f, &m);
    REQUIRE(m.active_slot == active, "active_slot moved from %d to %d", active, m.active_slot);
    REQUIRE(m.plan_state == PLAN_NONE && m.pending_slot == SLOT_NONE, "plan_state %d pending %d",
            m.plan_state, m.pending_slot);
    REQUIRE(format2_image_verify_flash(&f, active, 0u, desc_raw, &body) == FORMAT2_OK,
            "the running image no longer verifies after a forged self-targeting trial");
    flash_file_close(&f);
    return 0;
}

/* Orthogonal to the wire format: the meta bank's own HMAC (lib/flash.c), unaffected by the format2 migration beyond the harness it shares with everything above. */
static int t_meta_tampered(void)
{
    flash_t f;
    cap_t cap;
    device_session_t s;
    char path[1024];
    meta_t armed, after;
    uint8_t *img;
    size_t len;
    int live;
    uint32_t gen;

    REQUIRE(stage_armed(&f, &cap, &s, "meta_tamper", 2, 2) == 0, "cannot stage a plan");
    meta_load(&f, &armed);
    REQUIRE(armed.plan_state == PLAN_STAGING, "the plan is not on the books");
    flash_file_close(&f);

    /* Edits the live bank directly in the file, no key, no code path: a bank with the old CRC-32 scheme would only have needed its checksum redone. */
    snprintf(path, sizeof path, "%s/meta_tamper.bin", workdir);
    img = read_file(path, &len);
    REQUIRE(img, "cannot read the flash file");
    live = -1;
    gen = 0;
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
    meta_load(&f, &after);
    REQUIRE(after.generation < armed.generation, "the tampered bank was believed (generation %u)",
            after.generation);
    cap.log[0] = 0;
    REQUIRE(bootloader_boot(&f, cap_log, &cap, NULL) == 0, "boot: %s", f.err.msg);
    REQUIRE(!strstr(cap.log, "applying plan"), "the tampered plan was applied:\n%s", cap.log);
    flash_file_close(&f);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: auth_test WORKDIR BASE.bin\n"); return 2; }
    workdir = argv[1];
    base_path = argv[2];
    base_bin = read_file(base_path, &base_len);
    if (!base_bin) { fprintf(stderr, "cannot read %s\n", base_path); return 2; }

    static int (*const cases[])(void) = {
        t2_honest, t2_bad_seal, t2_bad_scheme, t2_step_order,
        t2_rollback_floor, t2_forged_trial, t_meta_tampered,
    };
    int bad = 0, n = (int)(sizeof cases / sizeof *cases);
    for (int i = 0; i < n; i++)
        if (cases[i]() < 0) bad++;
    free(base_bin);
    if (bad) { printf("FAIL  auth_test (%d of %d)\n", bad, n); return 1; }
    printf("PASS  auth_test (%d cases)\n", n);
    return 0;
}
