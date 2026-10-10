#include "image.h"
#include <string.h>

void image_hash(const uint8_t *slot, uint8_t out[SHA256_LEN])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, slot, VECT_LEN);
    sha256_update(&s, slot + CODE_OFF, SLOT_SIZE - CODE_OFF);
    sha256_final(&s, out);
}

const image_desc_t *image_desc(const uint8_t *slot)
{
    return (const image_desc_t *)(slot + DESC_OFF);
}

int image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                const uint8_t root_key[32], uint8_t out[SLOT_SIZE])
{
    image_desc_t d;

    if (binlen > IMG_MAX_PAYLOAD) return -1;

    memset(out, 0xFF, SLOT_SIZE);
    /* The descriptor sits between the vector table and the code, so the raw
     * binary is split around it. */
    size_t head = binlen < VECT_LEN ? binlen : VECT_LEN;
    memcpy(out, bin, head);
    if (binlen > VECT_LEN)
        memcpy(out + CODE_OFF, bin + VECT_LEN, binlen - VECT_LEN);

    memset(&d, 0, sizeof d);
    d.magic     = IMG_MAGIC;
    d.version   = version;
    d.image_len = SLOT_SIZE;
    d.flags     = 0;
    memcpy(out + DESC_OFF, &d, sizeof d);

    image_hash(out, d.sha256);
    /* The MAC covers the signed prefix: magic, version, length, flags, hash. */
    hmac_sha256(root_key, 32, &d, 16 + SHA256_LEN, d.mac);
    memcpy(out + DESC_OFF, &d, sizeof d);
    return 0;
}

int image_verify_hashed(const uint8_t *slot, const uint8_t root_key[32],
                        const uint8_t h[SHA256_LEN], const char **why)
{
    image_desc_t d;
    uint8_t mac[SHA256_LEN];

    memcpy(&d, slot + DESC_OFF, sizeof d);
    if (d.magic != IMG_MAGIC)      { *why = "no image descriptor"; return -1; }
    if (d.image_len != SLOT_SIZE)  { *why = "bad image_len";       return -1; }
    if (memcmp(h, d.sha256, SHA256_LEN)) { *why = "image_sha256 mismatch"; return -1; }

    hmac_sha256(root_key, 32, &d, 16 + SHA256_LEN, mac);
    if (memcmp(mac, d.mac, SHA256_LEN)) { *why = "release signature mismatch"; return -1; }

    *why = NULL;
    return 0;
}

int image_verify(const uint8_t *slot, const uint8_t root_key[32], const char **why)
{
    uint8_t h[SHA256_LEN];
    image_hash(slot, h);
    return image_verify_hashed(slot, root_key, h, why);
}
