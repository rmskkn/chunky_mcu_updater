#include "manifest.h"
#include <string.h>

void sector_hash(const void *sector, uint8_t out[SECTOR_HASH_LEN])
{
    uint8_t full[SHA256_LEN];
    sha256(sector, SECTOR_SIZE, full);
    memcpy(out, full, SECTOR_HASH_LEN);
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
    memcpy(a.manifest_digest, digest, SHA256_LEN);
    hmac_sha256(key, 32, &a, sizeof a, out);
}

/* With the order implicit there is nothing left to validate but the geometry:
 * a duplicated or missing sector is unrepresentable. */
int manifest_check(const manifest_t *m, const char **why)
{
    if (m->sector_size != SECTOR_SIZE) { *why = "sector_size"; return -1; }
    if (m->sectors != SECTORS_SLOT)  { *why = "sectors";     return -1; }
    *why = NULL;
    return 0;
}

const uint8_t *manifest_hash_at(const manifest_t *m, int index)
{
    return (index >= 0 && index < SECTORS_SLOT) ? m->h[index] : NULL;
}
