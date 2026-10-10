#include "manifest.h"
#include <string.h>

void manifest_build(flash_t *f, uint8_t active_slot, manifest_t *m)
{
    memset(m, 0, sizeof *m);
    m->sector_size  = PAGE_SIZE;
    m->slot_count   = SLOT_COUNT;
    m->sectors_slot = SECTORS_SLOT;
    m->active_slot  = active_slot;

    int k = 0;
    for (int s = 0; s < SLOT_COUNT; s++)
        for (int i = 0; i < SECTORS_SLOT; i++, k++) {
            m->e[k].slot  = (uint8_t)s;
            m->e[k].index = (uint8_t)i;
            flash_sector_md5(f, s, i, m->e[k].md5);
        }
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

int manifest_check(const manifest_t *m, const char **why)
{
    uint8_t seen[SLOT_COUNT][SECTORS_SLOT];

    if (m->sector_size != PAGE_SIZE)    { *why = "sector_size";  return -1; }
    if (m->slot_count != SLOT_COUNT)    { *why = "slot_count";   return -1; }
    if (m->sectors_slot != SECTORS_SLOT){ *why = "sectors_slot"; return -1; }
    if (m->active_slot >= SLOT_COUNT)   { *why = "active_slot";  return -1; }

    memset(seen, 0, sizeof seen);
    for (int k = 0; k < MANIFEST_ENTRIES; k++) {
        if (m->e[k].slot >= SLOT_COUNT)      { *why = "slot out of range";  return -1; }
        if (m->e[k].index >= SECTORS_SLOT)   { *why = "index out of range"; return -1; }
        if (seen[m->e[k].slot][m->e[k].index]) { *why = "duplicate sector"; return -1; }
        seen[m->e[k].slot][m->e[k].index] = 1;
    }
    for (int s = 0; s < SLOT_COUNT; s++)
        for (int i = 0; i < SECTORS_SLOT; i++)
            if (!seen[s][i]) { *why = "missing sector"; return -1; }

    *why = NULL;
    return 0;
}

const uint8_t *manifest_hash_at(const manifest_t *m, int slot, int index)
{
    for (int k = 0; k < MANIFEST_ENTRIES; k++)
        if (m->e[k].slot == slot && m->e[k].index == index)
            return m->e[k].md5;
    return NULL;
}

int manifest_find(const manifest_t *m, const uint8_t md5[MD5_LEN],
                  uint8_t *slot, uint8_t *index)
{
    for (int k = 0; k < MANIFEST_ENTRIES; k++)
        if (!memcmp(m->e[k].md5, md5, MD5_LEN)) {
            *slot  = m->e[k].slot;
            *index = m->e[k].index;
            return 0;
        }
    return -1;
}
