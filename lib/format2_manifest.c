#include "format2_manifest.h"
#include "format2_geom.h"
#include "format2.h"
#include "chunky_format.h"
#include <string.h>

/* SPEC.md 7.1: backend 1 = swap, matching this device's A/B copy apply path; duplicated from format2_geom.c's own FORMAT2_BACKEND since that constant isn't exported. */
#define FORMAT2_MANIFEST_BACKEND_SWAP 1u

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

static int sector_is_blank(flash_t *f, int slot, int index)
{
    uint8_t chunk[FLASH_READ_CHUNK];
    size_t o;

    for (o = 0; o < SECTOR_SIZE; o += sizeof chunk) {
        size_t j;
        flash_read(f, slot_base(slot) + (size_t)index * SECTOR_SIZE + o, chunk, sizeof chunk);
        for (j = 0; j < sizeof chunk; j++) {
            if (chunk[j] != 0xFFu) return 0;
        }
    }
    return 1;
}

/* One slot's FORMAT2_CHUNKS entries, ascending order (SPEC.md 7.2). Returns the new `at`, or 0 if out_cap ran out (0 is never valid here, since the header always precedes it). */
static size_t put_slot_entries(flash_t *f, int slot, uint8_t *out, size_t out_cap, size_t at)
{
    int i;

    for (i = 0; i < FORMAT2_CHUNKS; i++) {
        if (sector_is_blank(f, slot, i)) {
            if (at + 1u > out_cap) return 0;
            out[at] = CHUNKY_CHUNK_ENTRY_BLANK;
            at += 1u;
        } else {
            uint8_t hash[SECTOR_HASH_LEN];

            if (at + 1u + FORMAT2_CHUNK_HASH_LEN > out_cap) return 0;
            flash_sector_hash(f, slot, i, hash);
            out[at] = CHUNKY_CHUNK_ENTRY_HASH;
            /* flash_sector_hash() gives SECTOR_HASH_LEN bytes (8..32); format2's chunk hash is fixed at 8 (SPEC.md 2). Both are a prefix of the same SHA-256, so truncating here is correct at every SECTOR_HASH_LEN >= 8. */
            memcpy(out + at + 1u, hash, FORMAT2_CHUNK_HASH_LEN);
            at += 1u + FORMAT2_CHUNK_HASH_LEN;
        }
    }
    return at;
}

size_t format2_manifest_build(flash_t *f, int active_slot, int dfu_slot, uint8_t *out, size_t out_cap)
{
    size_t at;

    if (out_cap < CHUNKY_MANIFEST_HEADER_LEN) {
        return 0;
    }

    memcpy(out, "CKM2", 4u);
    out[4] = 2u;
    out[5] = (uint8_t)FORMAT2_MANIFEST_BACKEND_SWAP;
    out[6] = (uint8_t)CHUNKY_SCHEME_UNSIGNED;
    put_u32(out + 7, format2_geometry_id());
    put_u16(out + 11, (uint16_t)FORMAT2_CHUNKS);

    at = CHUNKY_MANIFEST_HEADER_LEN;
    at = put_slot_entries(f, active_slot, out, out_cap, at);
    if (at == 0u) return 0;
    at = put_slot_entries(f, dfu_slot, out, out_cap, at);
    if (at == 0u) return 0;

    return at;
}
