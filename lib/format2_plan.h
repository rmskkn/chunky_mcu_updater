#ifndef CB_FORMAT2_PLAN_H
#define CB_FORMAT2_PLAN_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "format2.h"

/* Server-side plan builder (ported from tools/format2.c's `gen`): ERASEs every block, then COPY/WRITE per chunk, skipping blanks (SPEC.md 5.1/5.4). Doesn't re-verify target's descriptor seal; the device does, via format2_plan_begin(). Returns total bytes written, or 0 on failure; never writes past out_cap. */
size_t format2_plan_build(const uint8_t active[SLOT_SIZE], const uint8_t target[SLOT_SIZE],
                          uint8_t *out, size_t out_cap);

/* Wrapper: reads ACTIVE into caller-owned active_scratch (SLOT_SIZE B; lib/ has no malloc, AGENTS.md), then calls format2_plan_build(). Returns 0 and sets *out_len on success, else -1 (reason in f->err). */
int format2_plan_build_from_active_slot(flash_t *f, int active_slot,
                                        const uint8_t target[SLOT_SIZE],
                                        uint8_t *active_scratch,
                                        uint8_t *out, size_t out_cap, size_t *out_len);

/* Device-side apply context: dfu_slot is being written; active_slot is COPY's source, and the slot format2_apply_step() refuses to touch (DESIGN.md 8's apply invariant). */
typedef struct {
    flash_t *f;
    int      dfu_slot;
    int      active_slot;
} format2_apply_ctx_t;

/* format2_step_fn (lib/format2.h), called once per step after its hash is already checked. ERASE erases the sector's units, WRITE programs its data, COPY re-reads ACTIVE's already-checked source and programs that. Returns 0, or -1 (reason in ctx's flash_t's err, including dfu_slot == active_slot). */
int format2_apply_step(void *ctx, const format2_step_t *step, uint32_t index);

/* format2_read_active_fn (lib/format2.h): reads chunk_len bytes of ACTIVE's chunk via flash_read(). Always returns 0; this flash model has no ECC-fault state to report. */
int format2_read_active(void *ctx, uint16_t chunk_index, uint8_t *buf, uint16_t chunk_len);

#endif
