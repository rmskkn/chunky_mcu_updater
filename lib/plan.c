#include "plan.h"
#include <string.h>

void plan_build(const manifest_t *dev, const uint8_t target[SLOT_SIZE],
                plan_t *p)
{
    uint8_t th[SECTORS_SLOT][SECTOR_HASH_LEN];
    uint8_t keep[SECTORS_SLOT];
    step_t tail;
    int have_tail = 0;

    memset(p, 0, sizeof *p);

    /* A sector the plan leaves alone is one whose bytes already are the
     * target's. Only those can be a COPY source: in place, WRITEs land
     * during the session and COPYs at boot, so a source that any step
     * rewrites no longer holds what the copy wants by then. */
    for (int i = 0; i < SECTORS_SLOT; i++) {
        sector_hash(target + (size_t)i * SECTOR_SIZE, th[i]);
        const uint8_t *at = manifest_hash_at(dev, i);
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

        int src = -1;
        for (int j = 0; j < SECTORS_SLOT && src < 0; j++)
            if (keep[j] && !memcmp(manifest_hash_at(dev, j), th[i], SECTOR_HASH_LEN))
                src = j;
        if (src >= 0) {
            s.op = OP_COPY;                            /* rule 2 */
            s.src_index = (sector_idx_t)src;
            p->n_copy++;
        } else {
            s.op = OP_WRITE;                           /* rule 3 */
            p->n_write++;
        }

        /* Sector 0 carries the descriptor and is the commit marker, so it is
         * always emitted last (DESIGN.md section 5). */
        if (i == 0) { tail = s; have_tail = 1; continue; }
        p->step[p->count++] = s;
    }
    if (have_tail) p->step[p->count++] = tail;
}
