#include "format2_image.h"
#include "format2_geom.h"
#include "chunky_format.h"
#include "sha256.h"
#include <string.h>

void format2_image_hash_flash(flash_t *f, int slot, uint8_t out[32])
{
    size_t base = slot_base(slot);
    uint8_t chunk[FLASH_READ_CHUNK];
    sha256_t s;
    size_t o;

    sha256_init(&s);
    /* FORMAT2_IMAGE_LEN need not be a multiple of FLASH_READ_CHUNK (the 128 B descriptor can leave a remainder), so each read is capped to what's left. */
    for (o = 0; o < (size_t)FORMAT2_IMAGE_LEN; o += sizeof chunk) {
        size_t n = (size_t)FORMAT2_IMAGE_LEN - o < sizeof chunk ? (size_t)FORMAT2_IMAGE_LEN - o : sizeof chunk;
        flash_read(f, base + o, chunk, n);
        sha256_update(&s, chunk, n);
    }
    sha256_final(&s, out);
}

format2_err_t format2_image_verify_flash(flash_t *f, int slot, uint32_t floor,
                                         uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN],
                                         chunky_descriptor_body_view_t *out_body)
{
    chunky_signed_descriptor_view_t desc;
    chunky_descriptor_body_view_t body;
    format2_err_t rc;
    uint8_t got[32];

    flash_read(f, slot_base(slot) + (size_t)FORMAT2_IMAGE_LEN, desc_raw, CHUNKY_SIGNED_DESCRIPTOR_LEN);
    /* desc_raw is always CHUNKY_SIGNED_DESCRIPTOR_LEN bytes (caller's contract), so this view only fails on a short buffer, which it never is; defensive, not reachable. */
    if (chunky_signed_descriptor_view(desc_raw, CHUNKY_SIGNED_DESCRIPTOR_LEN, &desc) != 0) {
        return FORMAT2_ESHORT;
    }

    rc = format2_descriptor_verify(desc, format2_geometry_id(), CB_PRODUCT_ID, floor,
                                   (uint32_t)FORMAT2_IMAGE_LEN, &body);
    if (rc != FORMAT2_OK) {
        return rc;
    }

    /* SPEC.md 4.3: the descriptor passed, but format2_descriptor_verify() never reads flash, so the image it describes still has to be checked against it. */
    format2_image_hash_flash(f, slot, got);
    if (memcmp(got, chunky_descriptor_body_image_sha256(body), 32u) != 0) {
        /* SPEC.md's "HashMismatch" (4.3 stage 3), distinct from the descriptor's own per-field refusals above. */
        return FORMAT2_EIMAGE;
    }

    if (out_body != NULL) {
        *out_body = body;
    }
    return FORMAT2_OK;
}

/* SPEC.md 4.2 seal domain, duplicated from lib/format2.c's private SEAL_DOMAIN: chunky_format.h has no encode side, so this has to match format2_descriptor_verify()'s check by hand. */
#define SEAL_DOMAIN "chunky image descriptor v2"
#define SEAL_DOMAIN_LEN 26u

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

int format2_image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                        uint32_t security_counter, uint8_t out[SLOT_SIZE], size_t *image_len)
{
    uint8_t *body;
    uint8_t *seal;
    uint8_t sha[32];
    uint8_t msg[SEAL_DOMAIN_LEN + CHUNKY_DESCRIPTOR_BODY_LEN];
    uint8_t sealhash[32];

    if (binlen > (size_t)FORMAT2_IMAGE_LEN) {
        return -1;
    }

    memset(out, 0xFF, SLOT_SIZE);
    memcpy(out, bin, binlen);
    /* out[binlen, FORMAT2_IMAGE_LEN) is already 0xFF from the memset. */

    sha256(out, (size_t)FORMAT2_IMAGE_LEN, sha);

    /* DescriptorBody layout (SPEC.md 4.1 / CHUNKY_DESCRIPTOR_BODY_* offsets): magic, format, scheme, reserved, product id, version, security counter, image length, geometry id, image SHA-256, reserved. */
    body = out + (size_t)FORMAT2_IMAGE_LEN;
    memset(body, 0, CHUNKY_DESCRIPTOR_BODY_LEN);
    memcpy(body + CHUNKY_DESCRIPTOR_BODY_MAGIC_OFFSET, "CKY2", CHUNKY_DESCRIPTOR_BODY_MAGIC_COUNT);
    body[CHUNKY_DESCRIPTOR_BODY_FORMAT_OFFSET] = 2u;
    body[CHUNKY_DESCRIPTOR_BODY_SCHEME_OFFSET] = (uint8_t)CHUNKY_SCHEME_UNSIGNED;
    /* reserved (offset 6, 2 bytes): zero, from the memset above */
    put_u32(body + CHUNKY_DESCRIPTOR_BODY_PRODUCT_ID_OFFSET, CB_PRODUCT_ID);
    put_u32(body + CHUNKY_DESCRIPTOR_BODY_VERSION_OFFSET, version);
    put_u32(body + CHUNKY_DESCRIPTOR_BODY_SECURITY_COUNTER_OFFSET, security_counter);
    put_u32(body + CHUNKY_DESCRIPTOR_BODY_IMAGE_LEN_OFFSET, (uint32_t)FORMAT2_IMAGE_LEN);
    put_u32(body + CHUNKY_DESCRIPTOR_BODY_GEOMETRY_ID_OFFSET, format2_geometry_id());
    memcpy(body + CHUNKY_DESCRIPTOR_BODY_IMAGE_SHA256_OFFSET, sha, 32u);
    /* reserved_end (offset 60, 4 bytes): zero, from the memset above */

    /* Seal (SPEC.md 4.2, scheme 2 unsigned): SHA-256(domain||body) then 32 zero bytes, the exact layout format2_descriptor_verify() checks. */
    seal = body + CHUNKY_DESCRIPTOR_BODY_LEN;
    memcpy(msg, SEAL_DOMAIN, SEAL_DOMAIN_LEN);
    memcpy(msg + SEAL_DOMAIN_LEN, body, CHUNKY_DESCRIPTOR_BODY_LEN);
    sha256(msg, sizeof msg, sealhash);
    memcpy(seal, sealhash, 32u);
    memset(seal + 32u, 0, 32u);

    *image_len = (size_t)FORMAT2_IMAGE_LEN;
    return 0;
}
