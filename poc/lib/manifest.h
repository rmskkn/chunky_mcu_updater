#ifndef CB_MANIFEST_H
#define CB_MANIFEST_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "proto.h"

/* Canonical manifest body, DESIGN.md section 4. Nonce-free on purpose: the
 * digest must be reproducible across sessions for the elision in 17.1.
 *
 * Entry order is implicit: sector i is at h[i], every sector always present.
 * That drops the index field, and with it the whole class of manifests that
 * repeat or omit a sector -- they cannot be expressed. */
typedef struct {
    uint16_t sector_size;
    uint16_t sectors;
    uint8_t  h[MANIFEST_ENTRIES][SECTOR_HASH_LEN];
} manifest_t;

#define MANIFEST_BODY_LEN (sizeof(manifest_t))

void manifest_build(flash_t *f, manifest_t *m);
void manifest_digest(const manifest_t *m, uint8_t out[SHA256_LEN]);
void manifest_mac(const uint8_t key[32], const uint8_t device_id[16],
                  const uint8_t nonce[NONCE_LEN], const uint8_t digest[SHA256_LEN],
                  uint8_t out[SHA256_LEN]);

/* Structural checks, DESIGN.md section 10.2 step 5. 0 = good. */
int  manifest_check(const manifest_t *m, const char **why);
/* Content-addressed lookup: finds any sector whose hash equals this one. */
int  manifest_find(const manifest_t *m, const uint8_t hash[SECTOR_HASH_LEN],
                   sector_idx_t *index);
const uint8_t *manifest_hash_at(const manifest_t *m, int index);

#endif
