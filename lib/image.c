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
