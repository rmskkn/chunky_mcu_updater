#include "bootloader.h"
#include "image.h"
#include "manifest.h"
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
    /* Only the inactive slot is ever erased: a meta that names anything
     * else is not a plan this bootloader wrote. */
    if (pending == OTHER_SLOT(m->active_slot)) {
        size_t off = slot_base(pending);
        for (int u = 0; u < SECTOR_SIZE / ERASE_SIZE; u++)
            if (flash_erase_unit(f, (int)(off / ERASE_SIZE) + u) < 0) return -1;
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

/* Whether the staged plan is the one the release key signed, checked before
 * the plan is applied: the descriptor in the pending slot is authenticated,
 * and the plan's dense table must fold to its table_root. The meta plan is
 * only ever believed through that root, so a forged or stale one never gets
 * to run a COPY. NULL if good, else the reason. */
static const char *plan_check(flash_t *f, const meta_t *m, const uint8_t root_key[32],
                              size_t *sha_bytes)
{
    image_desc_t d;
    uint8_t mac[SHA256_LEN], root[SHA256_LEN];

    if (m->pending_slot != OTHER_SLOT(m->active_slot)) return "pending slot is not the inactive slot";
    image_desc_read(f, m->pending_slot, &d);
    if (d.magic != IMG_MAGIC)                    return "no image descriptor";
    if (!image_len_ok(d.image_len) || d.image_len != m->image_len)
                                                 return "plan image_len differs from the descriptor's";
    image_desc_mac(&d, root_key, mac);
    if (memcmp(mac, d.mac, SHA256_LEN))          return "release signature mismatch";
    if (memcmp(d.sha256, m->target_sha256, SHA256_LEN)) return "descriptor differs from the plan";
    image_table_root((const uint8_t *)m->target_hash, d.image_len / SECTOR_SIZE, root);
    *sha_bytes += d.image_len / SECTOR_SIZE * SECTOR_HASH_LEN;
    if (memcmp(root, d.table_root, SHA256_LEN))  return "plan table does not match the signed table_root";
    return NULL;
}

/* image_hash_flash() over `slot` with the image_len its descriptor claims;
 * image_verify_flash() rejects a bad one. */
static void slot_hash(flash_t *f, int slot, size_t *sha_bytes, uint8_t out[SHA256_LEN])
{
    image_desc_t d;
    image_desc_read(f, slot, &d);
    if (image_hash_flash(f, slot, d.image_len, out) == 0 && sha_bytes)
        *sha_bytes += d.image_len - DESC_LEN;
}

/* Applies whatever the session staged into the pending slot, trial-boots
 * it, and otherwise decides whether the active slot is bootable (DESIGN.md
 * sections 7-8). See bootloader.h for the calling contract: one call is one
 * simulated boot/reset cycle. */
int bootloader_boot(flash_t *f, const uint8_t root_key[32],
                    log_fn log, void *log_ctx, device_stats_t *stats)
{
    device_stats_t dummy = { 0 };
    if (!stats) stats = &dummy;
    stats->t_apply = stats->t_verify = 0;
    stats->verify_bytes = 0;

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
        double t0 = stats_tick(stats);
        int bad = 0;
        const char *reject = plan_check(f, &m, root_key, &stats->sha_bytes);
        if (reject) {
            if (unroll_plan(f, &m, log, log_ctx, reject) < 0)
                return boot_flash_fail(f, log, log_ctx);
            goto final_boot;
        }
        int n_sectors = (int)(m.image_len / SECTOR_SIZE);
        for (int i = 0; i < n_sectors && !bad; i++) {
            if (m.op[i] == OP_NONE || journal_get(f, i) == JRN_DONE) continue;
            if (m.op[i] == OP_COPY && i == DESC_SECTOR) {
                /* The descriptor plan_check() just authenticated must stay. */
                if (unroll_plan(f, &m, log, log_ctx, "descriptor sector is a copy") < 0)
                    return boot_flash_fail(f, log, log_ctx);
                bad = 1;
                break;
            }

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
                image_sector_hash(i, tmp, got);
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
            image_sector_hash_flash(f, pending, i, got);
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
        stats->t_apply = stats_tick(stats) - t0;
        /* unroll_plan() already recorded and stored the result; boot
         * active_slot, which this failed apply never touched. */
        if (bad) goto final_boot;

        t0 = stats_tick(stats);
        uint8_t hsh[SHA256_LEN];
        image_desc_t pd;
        image_desc_read(f, pending, &pd);
        slot_hash(f, pending, &stats->verify_bytes, hsh);
        stats->sha_bytes += stats->verify_bytes;
        stats->ecdsa_ms += ECDSA_VERIFY_MS;
        if (pd.image_len != m.image_len)
            why = "image_len differs from the plan's";
        if (why || memcmp(hsh, m.target_sha256, SHA256_LEN) ||
            image_verify_flash(f, pending, root_key, hsh, &why) < 0) {
            stats->t_verify = stats_tick(stats) - t0;
            if (unroll_plan(f, &m, log, log_ctx,
                            why ? why : "image_sha256 mismatch") < 0)
                return boot_flash_fail(f, log, log_ctx);
            goto final_boot;
        }
        stats->t_verify = stats_tick(stats) - t0;

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
        if (m.attempts_left == 0) {
            if (revert_plan(f, &m, log, log_ctx) < 0) return boot_flash_fail(f, log, log_ctx);
            goto final_boot;   /* revert leaves nothing pending; boot active_slot */
        }
        m.attempts_left--;
        if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
        log_fmt(log, log_ctx,
               "boot: booting version %u (trial boot, slot %c, attempt %d/%d; "
               "call device_confirm() or this attempt is wasted)",
               image_version_flash(f, m.pending_slot), 'A' + m.pending_slot,
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
    slot_hash(f, m.active_slot, &stats->sha_bytes, hsh);
    if (image_verify_flash(f, m.active_slot, root_key, hsh, &why) < 0) {
        log_fmt(log, log_ctx, "boot: active slot %c does not verify (%s)",
               'A' + m.active_slot, why);
        int other = OTHER_SLOT(m.active_slot);
        uint8_t hsh2[SHA256_LEN];
        const char *why2 = NULL;
        slot_hash(f, other, NULL, hsh2);
        if (image_verify_flash(f, other, root_key, hsh2, &why2) == 0) {
            log_fmt(log, log_ctx, "boot: falling back to slot %c", 'A' + other);
            m.active_slot = (uint8_t)other;
            m.last_result = RES_VERIFY_FAILED;
            if (meta_store(f, &m) < 0) return boot_flash_fail(f, log, log_ctx);
            log_fmt(log, log_ctx, "boot: booting version %u", image_version_flash(f, m.active_slot));
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
        log_fmt(log, log_ctx, "boot: booting version %u", image_version_flash(f, m.active_slot));
    }
    return 0;
}
