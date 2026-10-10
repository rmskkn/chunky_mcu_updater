#ifndef CB_MANIFEST_H
#define CB_MANIFEST_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "proto.h"

/* Canonical manifest body, DESIGN.md section 4. Nonce-free on purpose: the
 * digest must be reproducible across sessions for the elision in 17.1. */
typedef struct {
    uint16_t sector_size;
    uint8_t  slot_count;
    uint8_t  sectors_slot;
    uint8_t  active_slot;
    uint8_t  pad[3];
    struct { uint8_t slot, index, md5[MD5_LEN]; } e[MANIFEST_ENTRIES];
} manifest_t;

#define MANIFEST_BODY_LEN (sizeof(manifest_t))

void manifest_build(flash_t *f, uint8_t active_slot, manifest_t *m);
void manifest_digest(const manifest_t *m, uint8_t out[SHA256_LEN]);
void manifest_mac(const uint8_t key[32], const uint8_t device_id[16],
                  const uint8_t nonce[NONCE_LEN], const uint8_t digest[SHA256_LEN],
                  uint8_t out[SHA256_LEN]);

/* Structural checks, DESIGN.md section 10.2 step 5. 0 = good. */
int  manifest_check(const manifest_t *m, const char **why);
/* Content-addressed lookup: finds any sector whose hash equals md5. */
int  manifest_find(const manifest_t *m, const uint8_t md5[MD5_LEN],
                   uint8_t *slot, uint8_t *index);
const uint8_t *manifest_hash_at(const manifest_t *m, int slot, int index);

#endif
