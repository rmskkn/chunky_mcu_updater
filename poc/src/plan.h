#ifndef CB_PLAN_H
#define CB_PLAN_H
#include <stdint.h>
#include "manifest.h"
#include "image.h"

typedef struct {
    uint8_t      op;          /* OP_COPY or OP_WRITE */
    uint8_t      pad;
    sector_idx_t dst;
    sector_idx_t src_index;
    uint8_t      hash[SECTOR_HASH_LEN];
} step_t;

typedef struct {
    step_t step[SECTORS_SLOT];
    int    count;
    int    n_skip, n_copy, n_write;
} plan_t;

/* The three rules of DESIGN.md section 5, against the one partition. */
void plan_build(const manifest_t *dev, const uint8_t target[SLOT_SIZE],
                plan_t *p);

#endif
