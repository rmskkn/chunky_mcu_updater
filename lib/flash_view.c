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
    m->sectors     = MANIFEST_ENTRIES;

    /* Both slots, slot A first then slot B (DESIGN.md section 4: "The
     * device hashes both slots and reports all of it"). */
    for (int i = 0; i < SECTORS_SLOT; i++) {
        flash_sector_hash(f, SLOT_A, i, m->h[manifest_index(SLOT_A, i)]);
        flash_sector_hash(f, SLOT_B, i, m->h[manifest_index(SLOT_B, i)]);
    }
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

void image_sector_hash_flash(flash_t *f, int slot, int index, uint8_t out[SECTOR_HASH_LEN])
{
    if (index != DESC_SECTOR) { flash_sector_hash(f, slot, index, out); return; }

    uint8_t full[SHA256_LEN];
    sha256_t s;
    size_t base = slot_base(slot) + (size_t)index * SECTOR_SIZE, at = DESC_OFF % SECTOR_SIZE;
    sha256_init(&s);
    hash_range(&s, f, base, at);
    hash_range(&s, f, base + at + DESC_LEN, SECTOR_SIZE - at - DESC_LEN);
    sha256_final(&s, full);
    memcpy(out, full, SECTOR_HASH_LEN);
}

int image_hash_flash(flash_t *f, int slot, uint32_t image_len, uint8_t out[SHA256_LEN])
{
    sha256_t s;
    size_t base = slot_base(slot);

    if (!image_len_ok(image_len)) { memset(out, 0, SHA256_LEN); return -1; }
    sha256_init(&s);
    hash_range(&s, f, base, VECT_LEN);
    hash_range(&s, f, base + CODE_OFF, image_len - CODE_OFF);
    sha256_final(&s, out);
    return 0;
}

void image_desc_read(flash_t *f, int slot, image_desc_t *d)
{
    flash_read(f, slot_base(slot) + DESC_OFF, d, sizeof *d);
}

uint32_t image_version_flash(flash_t *f, int slot)
{
    image_desc_t d;
    image_desc_read(f, slot, &d);
    return d.magic == IMG_MAGIC ? d.version : 0;
}

int image_verify_flash(flash_t *f, int slot, const uint8_t root_key[32],
                       const uint8_t h[SHA256_LEN], const char **why)
{
    image_desc_t d;
    uint8_t mac[SHA256_LEN];

    image_desc_read(f, slot, &d);
    if (d.magic != IMG_MAGIC)      { *why = "no image descriptor"; return -1; }
    if (!image_len_ok(d.image_len)) { *why = "bad image_len";     return -1; }
    if (memcmp(h, d.sha256, SHA256_LEN)) { *why = "image_sha256 mismatch"; return -1; }

    image_desc_mac(&d, root_key, mac);
    if (memcmp(mac, d.mac, SHA256_LEN)) { *why = "release signature mismatch"; return -1; }

    if (d.revoked == IMG_REVOKED) { *why = "image revoked after a failed trial boot"; return -1; }

    *why = NULL;
    return 0;
}

int image_revoke_flash(flash_t *f, int slot)
{
    uint32_t revoked = IMG_REVOKED;
    return flash_program(f, slot_base(slot) + DESC_OFF + offsetof(image_desc_t, revoked),
                         &revoked, sizeof revoked);
}
