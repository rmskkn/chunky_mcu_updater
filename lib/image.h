#ifndef CB_IMAGE_H
#define CB_IMAGE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"

/* Slot image layout, DESIGN.md section 10.1:
 *   0x000  512 B  vector table
 *   0x200  128 B  image descriptor
 *   0x280  ...    code and read-only data, 0xFF padded to the slot end
 * image_sha256 covers [0x000,0x200) then [0x280,SLOT_SIZE): the whole slot
 * with the descriptor excluded, so the hash is not self-referential. */
#define VECT_LEN   0x200
#define DESC_OFF   0x200
#define DESC_LEN   128
#define CODE_OFF   (DESC_OFF + DESC_LEN)
#define IMG_MAGIC  0x4D494243u  /* "CBIM" */

/* revoked is unsigned and outside image_sha256's domain (it sits after mac,
 * at 0x50, past the 0x30-byte signed prefix and the mac field) -- DESIGN.md
 * section 8 revert step 1 needs the bootloader to mark a slot REVOKED
 * in-place, after the image was already signed, without invalidating either
 * hash. docs/ab_migration.md's open question on slot-header revocation,
 * resolved here rather than with a new header: this field is never checked
 * by anything that trusts its value, only read as "never retry this slot
 * as-is" (image_verify_flash()). IMG_NOT_REVOKED is the erased value, so a
 * freshly built image needs no write to clear it, and revoking is a single
 * program of IMG_REVOKED (0x00000000) -- bits only ever clear, matching
 * every other field here that a live device writes without an erase. */
#define IMG_NOT_REVOKED 0xFFFFFFFFu
#define IMG_REVOKED     0x00000000u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t image_len;      /* always SLOT_SIZE */
    uint32_t flags;          /* bit0 allow_downgrade */
    uint8_t  sha256[SHA256_LEN];
    uint8_t  mac[SHA256_LEN];   /* stands in for ECDSA P-256 over the prefix */
    uint32_t revoked;           /* IMG_NOT_REVOKED, or IMG_REVOKED after a trial-failed revert */
    uint8_t  reserved[DESC_LEN - 16 - 2 * SHA256_LEN - 4];
} image_desc_t;

#define IMG_MAX_PAYLOAD (SLOT_SIZE - DESC_LEN)

void image_hash(const uint8_t *slot, uint8_t out[SHA256_LEN]);

/* Wraps a raw firmware binary into a slot image. Returns -1 if it is too big. */
int  image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                 const uint8_t root_key[32], uint8_t out[SLOT_SIZE]);

const image_desc_t *image_desc(const uint8_t *slot);

/* `slot`'s image, read through flash_read() in FLASH_READ_CHUNK pieces: the
 * device never holds a whole slot in RAM. */
void image_hash_flash(flash_t *f, int slot, uint8_t out[SHA256_LEN]);
void image_desc_read(flash_t *f, int slot, image_desc_t *d);
/* Checks slot's descriptor against h, a hash the caller already computed
 * with image_hash_flash(). 0 = good, else a reason string in *why. */
int  image_verify_flash(flash_t *f, int slot, const uint8_t root_key[32],
                        const uint8_t h[SHA256_LEN], const char **why);
/* DESIGN.md section 8 revert step 1: marks slot's descriptor REVOKED so
 * image_verify_flash() never again calls it bootable. */
int  image_revoke_flash(flash_t *f, int slot);

#endif
