#ifndef CB_PLAN_H
#define CB_PLAN_H
#include <stdint.h>
#include "manifest.h"
#include "image.h"

typedef struct {
    uint8_t      op;          /* OP_COPY or OP_WRITE */
    uint8_t      src_slot;    /* OP_COPY only: SLOT_A or SLOT_B */
    sector_idx_t dst;         /* destination-slot-relative */
    sector_idx_t src_index;   /* OP_COPY only: src_slot-relative */
} step_t;

typedef struct {
    step_t step[SECTORS_SLOT];
    int    count;
    int    n_skip, n_copy, n_write;
} plan_t;

/* Diffs only the image's own sectors, [0, image_len / SECTOR_SIZE): the
 * rest of dest_slot is outside the hash domain (DESIGN.md section 10.1), so
 * a smaller image leaves its stale tail alone.
 *
 * The three rules of DESIGN.md section 5, against dest_slot (always the
 * device's reported inactive slot) with active_slot as the other, untouched
 * one. The COPY search (rule 2) spans both slots: it always prefers
 * active_slot (always safe, read-only for the whole update, DESIGN.md
 * section 8) and falls back to dest_slot itself only for a sector that plan
 * also leaves alone there -- a same-slot copy whose source this same plan
 * rewrites is unsafe for the reason DESIGN.md section 18 gives for the
 * single-partition variant, and that reasoning still holds within one slot. */
void plan_build(const manifest_t *dev, int active_slot, int dest_slot,
                const uint8_t target[SLOT_SIZE], size_t image_len, plan_t *p);

#endif
