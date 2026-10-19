#ifndef CB_MANIFEST_H
#define CB_MANIFEST_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "protocol.h"

/* Canonical manifest body, DESIGN.md section 4. Nonce-free on purpose: the
 * digest must be reproducible across sessions for the elision in 17.1.
 *
 * Entry order is implicit: sector i is at h[i], every sector always present.
 * That drops the index field, and with it the whole class of manifests that
 * repeat or omit a sector -- they cannot be expressed.
 *
 * The device hashes both slots and reports all of it (DESIGN.md section 4):
 * slot A's sectors first, then slot B's, one flat array rather than (slot,
 * index) pairs -- docs/ab_migration.md's open question on indexing, resolved
 * this way because it is what's already on the wire (MANIFEST's hashes[] has
 * no slot field either) and manifest_index()/manifest_slot_of()/
 * manifest_sector_of() below are the only places that need to know the
 * split exists. */
typedef struct {
    uint16_t sector_size;
    uint16_t sectors;          /* 2 * SECTORS_SLOT */
    uint8_t  h[MANIFEST_ENTRIES][SECTOR_HASH_LEN];
} manifest_t;

#define MANIFEST_BODY_LEN (sizeof(manifest_t))

/* SECTOR_HASH_LEN bytes of SHA-256 over one sector in RAM (protocol.h). */
void sector_hash(const void *sector, uint8_t out[SECTOR_HASH_LEN]);

void manifest_build(flash_t *f, manifest_t *m);
void manifest_digest(const manifest_t *m, uint8_t out[SHA256_LEN]);
void manifest_mac(const uint8_t key[32], const uint8_t device_id[16],
                  const uint8_t nonce[NONCE_LEN], const uint8_t digest[SHA256_LEN],
                  uint8_t out[SHA256_LEN]);

/* Structural checks, DESIGN.md section 10.2 step 5. 0 = good. */
int  manifest_check(const manifest_t *m, const char **why);

/* Flat manifest index <-> (slot, slot-relative sector). */
static inline int manifest_index(int slot, int sector) {
    return slot == SLOT_A ? sector : SECTORS_SLOT + sector;
}
static inline int manifest_slot_of(int index)   { return index < SECTORS_SLOT ? SLOT_A : SLOT_B; }
static inline int manifest_sector_of(int index)  { return index < SECTORS_SLOT ? index : index - SECTORS_SLOT; }

const uint8_t *manifest_hash_at(const manifest_t *m, int index);

#endif
