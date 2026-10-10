#ifndef CB_PLAN_H
#define CB_PLAN_H
#include <stdint.h>
#include "manifest.h"
#include "image.h"

typedef struct {
    uint8_t op;          /* OP_COPY or OP_WRITE */
    uint8_t dst;
    uint8_t src_slot;
    uint8_t src_index;
    uint8_t md5[MD5_LEN];
} step_t;

typedef struct {
    step_t step[SECTORS_SLOT];
    int    count;
    int    n_skip, n_copy, n_write;
} plan_t;

/* The three rules of DESIGN.md section 5, against the DESTINATION slot.
 * allow_copy=0 models per-slot builds where rule 2 rarely fires.
 * force_all=1 is the full-image baseline: every sector becomes a write.
 * force_write is a sector index that must be sent as a write even if rule 1 or
 * rule 2 would cover it, or -1; the tamper test needs it. */
void plan_build(const manifest_t *dev, const uint8_t target[SLOT_SIZE],
                int dst_slot, int allow_copy, int force_all, int force_write,
                plan_t *p);

#endif
