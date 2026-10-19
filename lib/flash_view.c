/* Everything that reads the image and the manifest out of the device's
 * flash. Kept apart from image.c and manifest.c, which work on bytes in
 * RAM, so a server links those without needing a flash port. */
#include "image.h"
#include "manifest.h"
#include <string.h>

void manifest_build(flash_t *f, manifest_t *m)
{
    memset(m, 0, sizeof *m);
    m->sector_size = SECTOR_SIZE;
    m->sectors     = SECTORS_SLOT;

    for (int i = 0; i < SECTORS_SLOT; i++)
        flash_sector_hash(f, i, m->h[i]);
}

static void hash_range(sha256_t *s, flash_t *f, size_t off, size_t len)
{
    uint8_t chunk[FLASH_READ_CHUNK];
    while (len) {
        size_t n = len < sizeof chunk ? len : sizeof chunk;
        flash_read(f, off, chunk, n);
        sha256_update(s, chunk, n);
        off += n;
        len -= n;
    }
}

void image_hash_flash(flash_t *f, uint8_t out[SHA256_LEN])
{
    sha256_t s;
    sha256_init(&s);
    hash_range(&s, f, APP_BASE, VECT_LEN);
    hash_range(&s, f, APP_BASE + CODE_OFF, SLOT_SIZE - CODE_OFF);
    sha256_final(&s, out);
}

void image_desc_read(flash_t *f, image_desc_t *d)
{
    flash_read(f, APP_BASE + DESC_OFF, d, sizeof *d);
}

int image_verify_flash(flash_t *f, const uint8_t root_key[32],
                       const uint8_t h[SHA256_LEN], const char **why)
{
    image_desc_t d;
    uint8_t mac[SHA256_LEN];

    image_desc_read(f, &d);
    if (d.magic != IMG_MAGIC)      { *why = "no image descriptor"; return -1; }
    if (d.image_len != SLOT_SIZE)  { *why = "bad image_len";       return -1; }
    if (memcmp(h, d.sha256, SHA256_LEN)) { *why = "image_sha256 mismatch"; return -1; }

    hmac_sha256(root_key, 32, &d, 16 + SHA256_LEN, mac);
    if (memcmp(mac, d.mac, SHA256_LEN)) { *why = "release signature mismatch"; return -1; }

    *why = NULL;
    return 0;
}
