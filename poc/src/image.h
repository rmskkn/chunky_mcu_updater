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

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t image_len;      /* always SLOT_SIZE */
    uint32_t flags;          /* bit0 allow_downgrade */
    uint8_t  sha256[SHA256_LEN];
    uint8_t  mac[SHA256_LEN];   /* stands in for ECDSA P-256 over the prefix */
    uint8_t  reserved[DESC_LEN - 16 - 2 * SHA256_LEN];
} image_desc_t;

#define IMG_MAX_PAYLOAD (SLOT_SIZE - DESC_LEN)

void image_hash(const uint8_t *slot, uint8_t out[SHA256_LEN]);

/* Wraps a raw firmware binary into a slot image. Returns -1 if it is too big. */
int  image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                 const uint8_t root_key[32], uint8_t out[SLOT_SIZE]);

/* 0 = good, else a reason string in *why. */
int  image_verify(const uint8_t *slot, const uint8_t root_key[32], const char **why);

const image_desc_t *image_desc(const uint8_t *slot);

#endif
