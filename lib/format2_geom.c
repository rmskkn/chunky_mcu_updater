#include "format2_geom.h"

/* MIGRATION_FORMAT2.md's "Geometry reconciliation" table (SPEC.md 2: only equality is tested, each implementation derives its own); backend=1 matches the manifest header's own "swap" backend. */
#define FORMAT2_FAMILY  1u
#define FORMAT2_BACKEND 1u

/* FNV-1a 32 (SPEC.md 2); matches tools/gen_device.py's fnv1a32(), which computes CB_PRODUCT_ID's default the same way. */
static uint32_t fnv1a32_u32le(uint32_t h, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        h ^= (uint8_t)(v & 0xFFu);
        h *= 0x01000193u;
        v >>= 8;
    }
    return h;
}

uint32_t format2_geometry_id(void)
{
    uint32_t h = 0x811C9DC5u;

    h = fnv1a32_u32le(h, FORMAT2_FAMILY);
    h = fnv1a32_u32le(h, FORMAT2_BACKEND);
    h = fnv1a32_u32le(h, (uint32_t)FORMAT2_CHUNK_LEN);
    h = fnv1a32_u32le(h, (uint32_t)FORMAT2_ERASE_LEN);
    h = fnv1a32_u32le(h, (uint32_t)CB_FLASH_WRITE_UNIT);
    /* STATE: the rollback floor and plan state live inside meta_t, spanning both meta banks. */
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_META0_BASE);
    h = fnv1a32_u32le(h, (uint32_t)(CB_REGION_META0_SIZE + CB_REGION_META1_SIZE));
    /* FLOOR: same region as STATE; security_floor has no region of its own. */
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_META0_BASE);
    h = fnv1a32_u32le(h, (uint32_t)(CB_REGION_META0_SIZE + CB_REGION_META1_SIZE));
    /* ACTIVE/DFU: physical slot A/B, fixed, not the logical active/pending roles that swap on confirm. */
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_SLOT_A_BASE);
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_SLOT_A_SIZE);
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_SLOT_B_BASE);
    h = fnv1a32_u32le(h, (uint32_t)CB_REGION_SLOT_B_SIZE);

    return h;
}
