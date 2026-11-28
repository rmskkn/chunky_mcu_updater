#include "format2_plan.h"
#include "format2_geom.h"
#include "chunky_format.h"
#include <string.h>

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static int is_blank(const uint8_t *p, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0xFFu) return 0;
    }
    return 1;
}

/* First ACTIVE chunk matching target's bytes, -1 if none (SPEC.md 7.3's COPY-vs-WRITE rule); ported verbatim from tools/format2.c's find_active_match(). */
static long find_active_match(const uint8_t *active, uint32_t chunks, uint32_t chunk_len, const uint8_t *target)
{
    uint32_t i;
    for (i = 0; i < chunks; i++) {
        if (memcmp(active + (size_t)i * chunk_len, target, chunk_len) == 0) {
            return (long)i;
        }
    }
    return -1;
}

size_t format2_plan_build(const uint8_t active[SLOT_SIZE], const uint8_t target[SLOT_SIZE],
                          uint8_t *out, size_t out_cap)
{
    const uint32_t chunk_len = (uint32_t)FORMAT2_CHUNK_LEN;
    const uint32_t blocks = (uint32_t)FORMAT2_BLOCKS;
    const uint32_t chunks = (uint32_t)FORMAT2_CHUNKS;
    const uint32_t chunks_per_block = chunks / blocks; /* == 1, MIGRATION_FORMAT2.md */
    chunky_signed_descriptor_view_t descriptor;
    uint8_t *p, *body;
    uint32_t body_len, step_count = 0;
    uint32_t b, c;

    /* target's trailing 128 bytes are trusted (per this function's header comment) to already be a descriptor; this view only checks the length, not the seal. */
    if (chunky_signed_descriptor_view(target + (size_t)FORMAT2_IMAGE_LEN,
                                      SLOT_SIZE - (size_t)FORMAT2_IMAGE_LEN, &descriptor) != 0) {
        return 0;
    }
    if (out_cap < CHUNKY_PLAN_HEADER_LEN) {
        return 0;
    }

    body = out + CHUNKY_PLAN_HEADER_LEN;
    p = body;

    /* SPEC.md 5.4/7.3: ascending block then chunk, erasing every block (the "already right/blank" skip optimization is deferred per MIGRATION_FORMAT2.md) and skipping only a target chunk that's itself blank. */
    for (b = 0; b < blocks; b++) {
        if ((size_t)(p - out) + 3u > out_cap) return 0;
        *p++ = CHUNKY_STEP_ERASE;
        put_u16(p, (uint16_t)b);
        p += 2;
        step_count++;

        for (c = b * chunks_per_block; c < (b + 1u) * chunks_per_block; c++) {
            const uint8_t *tgt = target + (size_t)c * chunk_len;
            uint8_t hash[FORMAT2_CHUNK_HASH_LEN];
            long src;

            if (is_blank(tgt, chunk_len)) {
                continue;
            }

            format2_chunk_hash(tgt, chunk_len, hash);
            src = find_active_match(active, chunks, chunk_len, tgt);
            if (src >= 0) {
                if ((size_t)(p - out) + 13u > out_cap) return 0;
                *p++ = CHUNKY_STEP_COPY;
                put_u16(p, (uint16_t)c);
                p += 2;
                put_u16(p, (uint16_t)src);
                p += 2;
                memcpy(p, hash, FORMAT2_CHUNK_HASH_LEN);
                p += FORMAT2_CHUNK_HASH_LEN;
            } else {
                if ((size_t)(p - out) + 13u + chunk_len > out_cap) return 0;
                *p++ = CHUNKY_STEP_WRITE;
                put_u16(p, (uint16_t)c);
                p += 2;
                put_u16(p, (uint16_t)chunk_len);
                p += 2;
                memcpy(p, hash, FORMAT2_CHUNK_HASH_LEN);
                p += FORMAT2_CHUNK_HASH_LEN;
                memcpy(p, tgt, chunk_len);
                p += chunk_len;
            }
            step_count++;
        }
    }

    body_len = (uint32_t)(p - body);

    memcpy(out, "CKP2", 4u);
    out[4] = 2u;
    put_u32(out + 5, format2_geometry_id());
    put_u32(out + 9, step_count);
    put_u32(out + 13, body_len);
    memcpy(out + 17, descriptor.p, CHUNKY_SIGNED_DESCRIPTOR_LEN);

    return (size_t)CHUNKY_PLAN_HEADER_LEN + body_len;
}

int format2_plan_build_from_active_slot(flash_t *f, int active_slot,
                                        const uint8_t target[SLOT_SIZE],
                                        uint8_t *active_scratch,
                                        uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t got;

    if (flash_read(f, slot_base(active_slot), active_scratch, SLOT_SIZE) < 0) {
        return -1;
    }
    got = format2_plan_build(active_scratch, target, out, out_cap);
    if (got == 0u) {
        return err_set(&f->err, "format2: plan build failed (out_cap too small, or target's "
                       "trailing 128 bytes are not a format2 descriptor)");
    }
    *out_len = got;
    return 0;
}

int format2_read_active(void *ctx, uint16_t chunk_index, uint8_t *buf, uint16_t chunk_len)
{
    format2_apply_ctx_t *c = ctx;
    size_t off = slot_base(c->active_slot) + (size_t)chunk_index * chunk_len;

    flash_read(c->f, off, buf, chunk_len);
    return 0; /* this flash model has no "faults on ECC read" state */
}

int format2_apply_step(void *ctx, const format2_step_t *step, uint32_t index)
{
    format2_apply_ctx_t *c = ctx;
    (void)index;

    if (c->dfu_slot == c->active_slot) {
        return err_set(&c->f->err, "format2: refusing to touch slot %d, it is the active slot "
                       "(DESIGN.md section 8 apply invariant)", c->dfu_slot);
    }

    switch (step->op) {
    case CHUNKY_STEP_ERASE: {
        size_t off = slot_base(c->dfu_slot) + (size_t)step->a * (size_t)FORMAT2_ERASE_LEN;
        int u;

        for (u = 0; u < FORMAT2_ERASE_LEN / ERASE_SIZE; u++) {
            if (flash_erase_unit(c->f, (int)(off / ERASE_SIZE) + u) < 0) return -1;
        }
        return 0;
    }
    case CHUNKY_STEP_WRITE: {
        size_t off = slot_base(c->dfu_slot) + (size_t)step->a * (size_t)FORMAT2_CHUNK_LEN;
        return flash_program(c->f, off, step->data, step->b);
    }
    case CHUNKY_STEP_COPY: {
        uint8_t buf[FORMAT2_CHUNK_LEN];
        size_t off = slot_base(c->dfu_slot) + (size_t)step->a * (size_t)FORMAT2_CHUNK_LEN;

        /* format2_plan_walk() already checked this source chunk's hash via format2_read_active(); re-read here only gets the bytes to program. */
        if (flash_read(c->f, slot_base(c->active_slot) + (size_t)step->b * (size_t)FORMAT2_CHUNK_LEN,
                       buf, FORMAT2_CHUNK_LEN) < 0) {
            return -1;
        }
        return flash_program(c->f, off, buf, FORMAT2_CHUNK_LEN);
    }
    default:
        return err_set(&c->f->err, "format2: unknown step op %u", (unsigned)step->op);
    }
}
