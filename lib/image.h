#ifndef CB_IMAGE_H
#define CB_IMAGE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"

/* Slot image layout, DESIGN.md section 10.1:
 *   0x000  512 B  vector table
 *   0x200  128 B  image descriptor
 *   0x280  ...    code and read-only data, 0xFF padded to image_len
 * image_len is the real image size rounded up to a sector, at most
 * SLOT_SIZE. image_sha256 covers [0x000,0x200) then [0x280,image_len): the
 * image with the descriptor excluded, so the hash is not self-referential.
 * Bytes past image_len are outside every hash and never read. */
#define VECT_LEN   0x200
#define DESC_OFF   0x200
#define DESC_LEN   128
#define CODE_OFF   (DESC_OFF + DESC_LEN)
#define IMG_MAGIC  0x32494243u  /* "CBI2": the layout with table_root in the signed prefix */

/* The descriptor sits wholly inside one sector, so one step carries it. */
#define DESC_SECTOR (DESC_OFF / SECTOR_SIZE)
_Static_assert(DESC_SECTOR == (CODE_OFF - 1) / SECTOR_SIZE,
               "the image descriptor must not straddle two sectors");
_Static_assert(CODE_OFF <= SECTOR_SIZE, "an image must fit its header in one sector");

/* DESIGN.md section 10.1: a whole number of sectors holding at least the
 * header sector, and no more than the slot. */
static inline int image_len_ok(uint32_t len)
{
    return len >= CODE_OFF && len <= SLOT_SIZE && len % SECTOR_SIZE == 0;
}

/* revoked is unsigned and outside image_sha256's domain (it sits after mac,
 * past the signed prefix and the mac field) -- DESIGN.md
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

/* The signed prefix is everything before mac: magic .. table_root. */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t image_len;      /* image_len_ok(); sector multiple, not always SLOT_SIZE */
    uint32_t flags;          /* bit0 allow_downgrade */
    uint8_t  sha256[SHA256_LEN];
    uint8_t  table_root[SHA256_LEN];  /* SHA-256 over the image's per-sector table, DESIGN.md section 10.1 */
    uint8_t  mac[SHA256_LEN];   /* stands in for ECDSA P-256 over the prefix */
    uint32_t revoked;           /* IMG_NOT_REVOKED, or IMG_REVOKED after a trial-failed revert */
    uint8_t  reserved[DESC_LEN - 16 - 3 * SHA256_LEN - 4];
} image_desc_t;
_Static_assert(sizeof(image_desc_t) == DESC_LEN, "image_desc_t must fill the descriptor hole");

#define IMG_MAX_PAYLOAD (SLOT_SIZE - DESC_LEN)

/* image_sha256 over image[0, image_len) minus the descriptor hole. */
void image_hash(const uint8_t *image, size_t image_len, uint8_t out[SHA256_LEN]);

/* The per-sector hash the plan's table lists for sector `index` of an image:
 * the first SECTOR_HASH_LEN bytes of SHA-256, except that the descriptor
 * sector is hashed without the descriptor itself. table_root lives in the
 * descriptor and covers this table, so including the descriptor would make
 * the sector's hash depend on its own result. Everywhere else it is
 * sector_hash(). */
void image_sector_hash(int index, const uint8_t sector[SECTOR_SIZE],
                       uint8_t out[SECTOR_HASH_LEN]);
void image_sector_hash_flash(flash_t *f, int slot, int index, uint8_t out[SECTOR_HASH_LEN]);

/* The table of an image: image_len / SECTOR_SIZE entries of SECTOR_HASH_LEN,
 * dense and in sector order. table_root is the SHA-256 of that byte string. */
void image_table(const uint8_t *image, size_t image_len, uint8_t *table);
void image_table_root(const uint8_t *table, size_t n_sectors, uint8_t out[SHA256_LEN]);

/* HMAC under the release key over d's signed prefix (stands in for the
 * release signature). */
void image_desc_mac(const image_desc_t *d, const uint8_t root_key[32], uint8_t out[SHA256_LEN]);

/* Wraps a raw firmware binary into a slot image of *image_len bytes (a sector
 * multiple, 0xFF padded); out must hold SLOT_SIZE. Returns -1 if it is too big. */
int  image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                 const uint8_t root_key[32], uint8_t out[SLOT_SIZE], size_t *image_len);

const image_desc_t *image_desc(const uint8_t *slot);

/* `slot`'s image, read through flash_read() in FLASH_READ_CHUNK pieces: the
 * device never holds a whole slot in RAM. */
/* image_hash() over `slot`, with image_len from its descriptor (the caller
 * read it; it is not trusted until image_verify_flash()). -1 with out zeroed
 * if image_len is not image_len_ok(). */
int  image_hash_flash(flash_t *f, int slot, uint32_t image_len, uint8_t out[SHA256_LEN]);
void image_desc_read(flash_t *f, int slot, image_desc_t *d);
/* The slot's descriptor version, or 0 if it holds no image. */
uint32_t image_version_flash(flash_t *f, int slot);
/* Checks slot's descriptor against h, a hash the caller already computed
 * with image_hash_flash(). 0 = good, else a reason string in *why. */
int  image_verify_flash(flash_t *f, int slot, const uint8_t root_key[32],
                        const uint8_t h[SHA256_LEN], const char **why);
/* DESIGN.md section 8 revert step 1: marks slot's descriptor REVOKED so
 * image_verify_flash() never again calls it bootable. */
int  image_revoke_flash(flash_t *f, int slot);

#endif
