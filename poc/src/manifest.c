#include "manifest.h"
#include <string.h>

void manifest_build(flash_t *f, manifest_t *m)
{
    memset(m, 0, sizeof *m);
    m->sector_size = PAGE_SIZE;
    m->sectors     = SECTORS_SLOT;

    for (int i = 0; i < SECTORS_SLOT; i++)
        flash_sector_hash(f, i, m->h[i]);
}

void manifest_digest(const manifest_t *m, uint8_t out[SHA256_LEN])
{
    sha256(m, MANIFEST_BODY_LEN, out);
}

void manifest_mac(const uint8_t key[32], const uint8_t device_id[16],
                  const uint8_t nonce[NONCE_LEN], const uint8_t digest[SHA256_LEN],
                  uint8_t out[SHA256_LEN])
{
    manifest_auth_t a;
    memset(&a, 0, sizeof a);
    memcpy(a.domain, "CBMAN", 5);
    memcpy(a.device_id, device_id, 16);
    memcpy(a.nonce, nonce, NONCE_LEN);
    memcpy(a.manifest_sha256, digest, SHA256_LEN);
    hmac_sha256(key, 32, &a, sizeof a, out);
}

/* With the order implicit there is nothing left to validate but the geometry:
 * a duplicated or missing sector is unrepresentable. */
int manifest_check(const manifest_t *m, const char **why)
{
    if (m->sector_size != PAGE_SIZE) { *why = "sector_size"; return -1; }
    if (m->sectors != SECTORS_SLOT)  { *why = "sectors";     return -1; }
    *why = NULL;
    return 0;
}

const uint8_t *manifest_hash_at(const manifest_t *m, int index)
{
    return (index >= 0 && index < SECTORS_SLOT) ? m->h[index] : NULL;
}

int manifest_find(const manifest_t *m, const uint8_t hash[SECTOR_HASH_LEN],
                  sector_idx_t *index)
{
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (!memcmp(m->h[i], hash, SECTOR_HASH_LEN)) {
            *index = (sector_idx_t)i;
            return 0;
        }
    return -1;
}
