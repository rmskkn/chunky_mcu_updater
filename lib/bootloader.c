#include "bootloader.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_image.h"
#include "sha256.h"
#include "util.h"
#include <string.h>

/* The bootloader could not record what it found: the meta banks keep their
 * last good state, and the next boot sees the same plan again. */
static int boot_flash_fail(flash_t *f, log_fn log, void *log_ctx)
{
    log_fmt(log, log_ctx, "boot: cannot record the result: %s", f->err.msg);
    return -1;
}

/* Erases format2's trailing descriptor (SPEC.md 4: last 128 B, in the slot's last sector); unlike the old layout, that descriptor isn't at a fixed low offset, so erasing sector 0 no longer invalidates it. */
static int erase_format2_descriptor(flash_t *f, int slot)
{
    size_t off = slot_base(slot) + (size_t)SLOT_SIZE - SECTOR_SIZE;
    for (int u = 0; u < SECTOR_SIZE / ERASE_SIZE; u++)
        if (flash_erase_unit(f, (int)(off / ERASE_SIZE) + u) < 0) return -1;
    return 0;
}

/* Unroll (DESIGN.md section 8): abandon a plan that never booted -- never
 * armed, a copy source gone stale, or the staged image failing verify.
 * Erasing the pending slot's trailing descriptor invalidates the image; active_slot stays untouched until a confirm that now never happens. */
static int unroll_plan(flash_t *f, meta_t *m, log_fn log, void *log_ctx, const char *why)
{
    int pending = m->pending_slot;
    log_fmt(log, log_ctx, "boot: unrolling the plan into slot %c (%s)", 'A' + pending, why);
    /* Only the inactive slot is ever erased: a meta that names anything
     * else is not a plan this bootloader wrote. */
    if (pending == OTHER_SLOT(m->active_slot)) {
        if (erase_format2_descriptor(f, pending) < 0) return -1;
    }
    m->plan_state    = PLAN_NONE;
    m->pending_slot  = SLOT_NONE;
    m->attempts_left = 0;
    m->last_result   = RES_VERIFY_FAILED;
    return meta_store(f, m);
}

/* Revert (DESIGN.md section 8): a committed-but-broken image, after
 * MAX_TRIES failed trial boots. active_slot is unchanged -- it was never
 * moved, so the image that was running before the update is still there.
 * format2 has no revoked field (SPEC.md 4.1), so the pending descriptor is erased the same way unroll_plan() does, leaving the slot free for the next plan. */
static int revert_plan(flash_t *f, meta_t *m, log_fn log, void *log_ctx)
{
    int pending = m->pending_slot;
    if (pending == OTHER_SLOT(m->active_slot)) {
        log_fmt(log, log_ctx, "boot: reverting after %d failed trial boots; "
               "slot %c revoked, staying on slot %c",
               MAX_TRIES, 'A' + pending, 'A' + m->active_slot);
        if (erase_format2_descriptor(f, pending) < 0) return -1;
    } else {
        /* Never revoke the running image: a meta naming any other slot is
         * not a plan this bootloader trial-booted. */
        log_fmt(log, log_ctx, "boot: dropping a trial plan whose pending slot is not "
               "the inactive slot; staying on slot %c", 'A' + m->active_slot);
    }
    m->plan_state    = PLAN_NONE;
    m->pending_slot  = SLOT_NONE;
    m->attempts_left = 0;
    m->last_result   = RES_TRIAL_FAILED;
    return meta_store(f, m);
}

/* Whole-slot format2 check (SPEC.md 4.3) plus anti-rollback (section 12.4); desc_raw is caller-owned storage that out_body points into, so it must outlive out_body's use. NULL if good, else the reason. */
static const char *pending_image_check(flash_t *f, const meta_t *m, size_t *sha_bytes,
                                       uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN],
                                       chunky_descriptor_body_view_t *out_body)
{
    format2_err_t e;

    if (m->pending_slot != OTHER_SLOT(m->active_slot)) return "pending slot is not the inactive slot";
    e = format2_image_verify_flash(f, m->pending_slot, m->security_floor, desc_raw, out_body);
    if (sha_bytes) *sha_bytes += (size_t)FORMAT2_IMAGE_LEN;
    return e == FORMAT2_OK ? NULL : format2_err_str(e);
}

/* Gate before STAGING->TRIAL (DESIGN.md 7.3): under format2 every sector was already written and hash-checked live during the session, so this is effectively the only real check that transition makes. */
static const char *plan_check(flash_t *f, const meta_t *m, size_t *sha_bytes)
{
    uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
    chunky_descriptor_body_view_t body;

    return pending_image_check(f, m, sha_bytes, desc_raw, &body);
}

/* The whole pending image, before it may run (DESIGN.md section 7 step 3).
 * Runs at STAGING->TRIAL and every later trial boot; NULL if good, else the reason. No ecdsa_ms: format2 scheme 2 is unsigned (SPEC.md 4.2), nothing asymmetric to verify. */
static const char *pending_verify(flash_t *f, const meta_t *m, device_stats_t *stats)
{
    uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
    chunky_descriptor_body_view_t body;
    size_t hashed = 0;
    const char *why = pending_image_check(f, m, &hashed, desc_raw, &body);

    stats->verify_bytes += hashed;
    stats->sha_bytes    += hashed;
    return why;
}

/* Applies whatever the session staged into the pending slot, trial-boots
 * it, and otherwise decides whether the active slot is bootable (DESIGN.md
 * sections 7-8). See bootloader.h for the calling contract: one call is one
 * simulated boot/reset cycle. No key parameter: format2 (scheme 2, SPEC.md 4.2) is unsigned, so none is needed. */
int bootloader_boot(flash_t *f, log_fn log, void *log_ctx, device_stats_t *stats)
{
    device_stats_t dummy = { 0 };
    if (!stats) stats = &dummy;
    stats->t_apply = stats->t_verify = 0;
    stats->verify_bytes = 0;

    meta_t m;
    const char *why = NULL;
    int verified = 0;   /* pending image checked earlier in this same call */

    meta_load(f, &m);

    if (m.plan_state == PLAN_STAGING && journal_get(f, JRN_ARM_INDEX) != 0x0000) {
        if (unroll_plan(f, &m, log, log_ctx, "plan was never committed") < 0)
            return boot_flash_fail(f, log, log_ctx);
        goto final_boot;   /* unroll leaves nothing pending; boot active_slot */
    } else if (m.plan_state == PLAN_STAGING) {
        int pending = m.pending_slot;
        log_fmt(log, log_ctx, "boot: applying plan v%u into slot %c",
               m.target_version, 'A' + pending);
        double t0 = stats_tick(stats);
        const char *reject = plan_check(f, &m, &stats->sha_bytes);
        stats->t_apply = stats_tick(stats) - t0;
        if (reject) {
            if (unroll_plan(f, &m, log, log_ctx, reject) < 0)
                return boot_flash_fail(f, log, log_ctx);
            goto final_boot;
        }
        /* No per-sector apply loop: format2 already wrote and hash-checked every sector live during the session (lib/device.c's format2_plan_walk()/format2_apply_step()), so plan_check() above is the only real gate this transition makes. */

        t0 = stats_tick(stats);
        why = pending_verify(f, &m, stats);
        stats->t_verify = stats_tick(stats) - t0;
        if (why) {
            if (unroll_plan(f, &m, log, log_ctx, why) < 0)
                return boot_flash_fail(f, log, log_ctx);
            goto final_boot;
        }
        verified = 1;

        /* DESIGN.md section 7 steps 4-5: the pending image verified, so it
         * becomes a trial -- MAX_TRIES tokens, then immediately consume one
         * for this same boot/reset cycle (see bootloader.h's calling contract:
         * the STAGING->TRIAL transition and the first trial attempt happen
         * in one bootloader_boot() call, exactly like a real bootloader would
         * do between one apply pass and the jump that follows it). */
        m.plan_state    = PLAN_TRIAL;
        m.attempts_left = MAX_TRIES;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
    }

    if (m.plan_state == PLAN_TRIAL) {
        if (!verified) {
            double t0 = stats_tick(stats);
            why = pending_verify(f, &m, stats);
            stats->t_verify = stats_tick(stats) - t0;
            if (why) {
                if (unroll_plan(f, &m, log, log_ctx, why) < 0)
                    return boot_flash_fail(f, log, log_ctx);
                why = NULL;
                goto final_boot;
            }
        }
        if (m.attempts_left == 0) {
            if (revert_plan(f, &m, log, log_ctx) < 0) return boot_flash_fail(f, log, log_ctx);
            goto final_boot;   /* revert leaves nothing pending; boot active_slot */
        }
        m.attempts_left--;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
        log_fmt(log, log_ctx,
               "boot: booting version %u (trial boot, slot %c, attempt %d/%d; "
               "call device_confirm() or this attempt is wasted)",
               m.target_version, 'A' + m.pending_slot,
               MAX_TRIES - m.attempts_left, MAX_TRIES);
        /* Unlike every other path here, this call ends with the *pending*
         * slot running on trial, not active_slot -- there is nothing to
         * fall through to. */
        return 0;
    }

final_boot:
    /* plan_state is NONE or COMMITTED: an ordinary boot of active_slot.
     * format2_image_verify_flash() (SPEC.md 4.3) re-checks every boot, catching bit rot or an old reflashed image; a blank 0xFF slot fails the magic check cleanly (FORMAT2_EMAGIC) rather than causing UB. */
    uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
    chunky_descriptor_body_view_t body;
    format2_err_t e = format2_image_verify_flash(f, m.active_slot, m.security_floor, desc_raw, &body);
    stats->sha_bytes += (size_t)FORMAT2_IMAGE_LEN;
    if (e != FORMAT2_OK) {
        why = format2_err_str(e);
        log_fmt(log, log_ctx, "boot: active slot %c does not verify (%s)",
               'A' + m.active_slot, why);
        int other = OTHER_SLOT(m.active_slot);
        uint8_t desc_raw2[CHUNKY_SIGNED_DESCRIPTOR_LEN];
        chunky_descriptor_body_view_t body2;
        format2_err_t e2 = format2_image_verify_flash(f, other, m.security_floor, desc_raw2, &body2);
        if (e2 == FORMAT2_OK) {
            log_fmt(log, log_ctx, "boot: falling back to slot %c", 'A' + other);
            m.active_slot = (uint8_t)other;
            m.last_result = RES_VERIFY_FAILED;
            if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
            log_fmt(log, log_ctx, "boot: booting version %u", chunky_descriptor_body_version(body2));
        } else {
            /* Both slots unusable: this is as far as the device gets. A
             * real port enters the recovery loop here (DESIGN.md section
             * 3); the PoC just reports it and asks for another release. */
            log_fmt(log, log_ctx, "boot: no bootable image in either slot (%s); "
                   "requesting an update", format2_err_str(e2));
            if (m.last_result != RES_VERIFY_FAILED) {
                m.last_result = RES_VERIFY_FAILED;
                if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
            }
        }
    } else {
        log_fmt(log, log_ctx, "boot: booting version %u", chunky_descriptor_body_version(body));
    }
    return 0;
}
