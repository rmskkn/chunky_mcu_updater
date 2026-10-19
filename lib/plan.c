#include "plan.h"
#include <string.h>

void plan_build(const manifest_t *dev, int active_slot, int dest_slot,
                const uint8_t target[SLOT_SIZE], plan_t *p)
{
    uint8_t th[SECTORS_SLOT][SECTOR_HASH_LEN];
    uint8_t keep[SECTORS_SLOT];     /* dest_slot's own sector i already == th[i] */
    step_t tail;
    int have_tail = 0;

    memset(p, 0, sizeof *p);

    /* Baseline is dest_slot's current content (DESIGN.md section 5: "the
     * comparison is per destination sector, against what the device reports
     * in the destination slot"), not active_slot's -- the destination is
     * what has to end up byte-identical to the target. A sector dest_slot
     * already holds correctly is also the only kind of same-slot COPY
     * source this plan can safely use later: WRITEs land during the session
     * and COPYs at boot, so a dest_slot source any step of this plan
     * rewrites no longer holds what the copy wants by the time the
     * bootloader gets to it. */
    for (int i = 0; i < SECTORS_SLOT; i++) {
        sector_hash(target + (size_t)i * SECTOR_SIZE, th[i]);
        const uint8_t *at = manifest_hash_at(dev, manifest_index(dest_slot, i));
        keep[i] = at && !memcmp(at, th[i], SECTOR_HASH_LEN);
    }

    for (int i = 0; i < SECTORS_SLOT; i++) {
        if (keep[i]) {
            p->n_skip++;                               /* rule 1 */
            continue;
        }
        step_t s;
        memset(&s, 0, sizeof s);
        s.dst = (sector_idx_t)i;
        memcpy(s.hash, th[i], SECTOR_HASH_LEN);

        /* rule 2: active_slot first -- it is read-only for the whole
         * update, so any match there is always a safe source, unlike one in
         * dest_slot itself. */
        int src = -1;
        for (int j = 0; j < SECTORS_SLOT && src < 0; j++)
            if (!memcmp(manifest_hash_at(dev, manifest_index(active_slot, j)), th[i], SECTOR_HASH_LEN))
                src = j;
        if (src >= 0) {
            s.op = OP_COPY;
            s.src_slot  = (uint8_t)active_slot;
            s.src_index = (sector_idx_t)src;
            p->n_copy++;
        } else {
            for (int j = 0; j < SECTORS_SLOT && src < 0; j++)
                if (keep[j] && !memcmp(manifest_hash_at(dev, manifest_index(dest_slot, j)),
                                       th[i], SECTOR_HASH_LEN))
                    src = j;
            if (src >= 0) {
                s.op = OP_COPY;
                s.src_slot  = (uint8_t)dest_slot;
                s.src_index = (sector_idx_t)src;
                p->n_copy++;
            } else {
                s.op = OP_WRITE;                        /* rule 3 */
                p->n_write++;
            }
        }

        /* Sector 0 carries the descriptor and is the commit marker, so it is
         * always emitted last (DESIGN.md section 5). */
        if (i == 0) { tail = s; have_tail = 1; continue; }
        p->step[p->count++] = s;
    }
    if (have_tail) p->step[p->count++] = tail;
}
