#include "image.h"
#include <string.h>

void image_hash(const uint8_t *image, size_t image_len, uint8_t out[SHA256_LEN])
{
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, image, VECT_LEN);
    sha256_update(&s, image + CODE_OFF, image_len - CODE_OFF);
    sha256_final(&s, out);
}

void image_sector_hash(int index, const uint8_t sector[SECTOR_SIZE], uint8_t out[SECTOR_HASH_LEN])
{
    uint8_t full[SHA256_LEN];
    sha256_t s;

    sha256_init(&s);
    if (index == DESC_SECTOR) {
        size_t at = DESC_OFF % SECTOR_SIZE;
        sha256_update(&s, sector, at);
        sha256_update(&s, sector + at + DESC_LEN, SECTOR_SIZE - at - DESC_LEN);
    } else {
        sha256_update(&s, sector, SECTOR_SIZE);
    }
    sha256_final(&s, full);
    memcpy(out, full, SECTOR_HASH_LEN);
}

void image_table(const uint8_t *image, size_t image_len, uint8_t *table)
{
    for (size_t i = 0; i < image_len / SECTOR_SIZE; i++)
        image_sector_hash((int)i, image + i * SECTOR_SIZE, table + i * SECTOR_HASH_LEN);
}

void image_table_root(const uint8_t *table, size_t n_sectors, uint8_t out[SHA256_LEN])
{
    sha256(table, n_sectors * SECTOR_HASH_LEN, out);
}

void image_desc_mac(const image_desc_t *d, const uint8_t root_key[32], uint8_t out[SHA256_LEN])
{
    hmac_sha256(root_key, 32, d, offsetof(image_desc_t, mac), out);
}

const image_desc_t *image_desc(const uint8_t *slot)
{
    return (const image_desc_t *)(slot + DESC_OFF);
}

int image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                const uint8_t root_key[32], uint8_t out[SLOT_SIZE], size_t *image_len)
{
    image_desc_t d;

    if (binlen > IMG_MAX_PAYLOAD) return -1;
    size_t used = binlen > VECT_LEN ? binlen + DESC_LEN : CODE_OFF;
    size_t len = (used + SECTOR_SIZE - 1) / SECTOR_SIZE * SECTOR_SIZE;

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
    d.image_len = (uint32_t)len;
    d.flags     = 0;
    d.revoked   = IMG_NOT_REVOKED;   /* erased value: nothing to clear later unless reverted */
    memcpy(out + DESC_OFF, &d, sizeof d);

    image_hash(out, len, d.sha256);
    uint8_t table[SECTORS_SLOT * SECTOR_HASH_LEN];
    image_table(out, len, table);
    image_table_root(table, len / SECTOR_SIZE, d.table_root);
    image_desc_mac(&d, root_key, d.mac);
    memcpy(out + DESC_OFF, &d, sizeof d);
    *image_len = len;
    return 0;
}
