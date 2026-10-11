#include "plan.h"
#include <string.h>

void plan_build(const manifest_t *dev, const uint8_t target[SLOT_SIZE],
                plan_t *p)
{
    step_t tail;
    int have_tail = 0;

    memset(p, 0, sizeof *p);

    for (int i = 0; i < SECTORS_SLOT; i++) {
        uint8_t h[MD5_LEN];
        md5(target + (size_t)i * PAGE_SIZE, PAGE_SIZE, h);

        const uint8_t *at = manifest_hash_at(dev, i);
        step_t s;
        memset(&s, 0, sizeof s);
        s.dst = (sector_idx_t)i;
        memcpy(s.md5, h, MD5_LEN);

        if (at && !memcmp(at, h, MD5_LEN)) {
            p->n_skip++;                               /* rule 1 */
            continue;
        }
        sector_idx_t si;
        if (manifest_find(dev, h, &si) == 0) {
            s.op = OP_COPY;                            /* rule 2 */
            s.src_index = si;
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
