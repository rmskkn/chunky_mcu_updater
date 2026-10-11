#ifndef CB_FLASH_H
#define CB_FLASH_H
#include <stdint.h>
#include <stddef.h>
#include "md5.h"
#include "sha256.h"

/* Flash geometry, STM32F103CB as in DESIGN.md section 2 / 10.2 / 18.
 *
 * One application partition, updated in place. There is no second slot, so
 * there is nothing to fall back to: an update that fails to verify leaves the
 * device without a bootable image and the bootloader asks for another one.
 * That also means the running image cannot do the writing -- it would be
 * overwriting itself -- so the update belongs to the bootloader. */
#define PAGE_SIZE     1024
#define PAGE_COUNT    128
#define FLASH_SIZE    (PAGE_SIZE * PAGE_COUNT)

#define BL_PAGE       0
#define BL_PAGES      15
#define ID_PAGE       15
#define META0_PAGE    16
#define META1_PAGE    20
#define META_PAGES    4
#define APP_PAGE      24
#define APP_PAGES     104

#define SLOT_SIZE     (APP_PAGES * PAGE_SIZE)
#define SECTORS_SLOT  APP_PAGES
#define APP_BASE      ((size_t)APP_PAGE * PAGE_SIZE)
#define MANIFEST_ENTRIES SECTORS_SLOT

/* Sector indices are partition-relative. A byte covers 256 sectors, which is
 * a 256 KB partition at a 1 KB page and runs out on a 1 MB part, so the index
 * has its own type everywhere it appears: in the manifest, on the wire, and in
 * the plan the bootloader reads back. */
typedef uint16_t sector_idx_t;

/* Per-sector hashes are MD5 truncated to 8 bytes. They answer one question --
 * does this sector already hold the bytes the target needs -- and they are not
 * a trust boundary: image_sha256 and the release signature gate the result, so
 * a collision costs a retried update rather than a corrupt device. At 64 bits
 * the chance of a false match is about 3e-16 for 104 sectors and 3e-14 for
 * 1000. Halving the hash halves the manifest and the stored plan, which is
 * where the bytes actually are. */
#define SECTOR_HASH_LEN 8

void sector_hash(const void *page, uint8_t out[SECTOR_HASH_LEN]);

/* Three things grow with the number of sectors, and only one of them is the
 * index width. The asserts below turn each cliff into a build error rather
 * than silent corruption on a bigger part. */
_Static_assert(SECTORS_SLOT <= (1 << (8 * sizeof(sector_idx_t))) - 1,
               "sector index type too narrow for this partition size");

/* Journal states. Every transition only clears bits, which is what an
 * STM32F103 half-word program can do without erasing the page first. */
#define JRN_EMPTY     0xFFFFu
#define JRN_WRITTEN   0xFFF0u
#define JRN_VERIFIED  0xFF00u
#define JRN_DONE      0x0000u

/* plan_state. Without a second partition there is no trial boot and no
 * revert: the image either verifies or the device asks for another one. */
enum { PLAN_NONE = 0, PLAN_STAGING, PLAN_APPLYING, PLAN_COMMITTED, PLAN_BAD };

/* step ops */
enum { OP_NONE = 0, OP_COPY, OP_WRITE };

#define ID_MAGIC   0x44494243u  /* "CBID" */
#define META_MAGIC 0x4154454Du  /* "META" */

typedef struct {
    uint32_t magic;
    uint8_t  device_id[16];
    uint8_t  dev_key[32];   /* manifest MAC key (stands in for ECDSA priv) */
    uint8_t  root_key[32];  /* release MAC key  (stands in for the root pubkey) */
} identity_t;

typedef struct {
    uint32_t magic;
    uint32_t generation;
    uint32_t crc;            /* over everything that follows */
    uint8_t  plan_state;
    uint8_t  pad[3];
    uint32_t target_version;
    uint8_t  target_sha256[SHA256_LEN];
    uint8_t  target_mac[SHA256_LEN];
    uint16_t step_count;
    uint8_t  op[SECTORS_SLOT];
    sector_idx_t src_index[SECTORS_SLOT];
    uint8_t  target_hash[SECTORS_SLOT][SECTOR_HASH_LEN];
    uint32_t last_result;
} meta_t;

/* Bank layout: meta_t first, then the progress journal in a page of its own.
 * The journal is advanced by clearing bits and must never be erased while a
 * plan is live, so it cannot share a page with anything. */
#define META_PAGES_HDR  3                       /* pages meta_t may use */
#define JOURNAL_OFF     (META_PAGES_HDR * PAGE_SIZE)

/* meta_t carries ~20 B per sector, so it grows with the partition. These two
 * are the boundaries it has to respect: a 1 MB part at a 1 KB page would need
 * 10 KB of plan and fits neither. */
_Static_assert(sizeof(meta_t) <= JOURNAL_OFF,
               "meta_t runs into the journal page: use fewer, larger sectors, "
               "or give the bank more pages");
_Static_assert(META_PAGES_HDR < META_PAGES,
               "no page left in the bank for the journal");
_Static_assert((SECTORS_SLOT + 3) * 2 <= PAGE_SIZE,
               "the progress journal does not fit its page");

/* last_result codes reported back in HELLO */
enum { RES_NONE = 0, RES_OK, RES_VERIFY_FAILED, RES_FLASH_FAIL };

typedef struct {
    uint8_t *map;
    int      fd;
    long     n_erase;      /* page erases, for the device time model */
    long     n_prog;
    size_t   prog_bytes;
    const char *path;
} flash_t;

/* STM32F103 flash timings, datasheet DS5319 Table 23 "Flash memory
 * characteristics", TA = -40 to +105 C:
 *
 *   tprog   Word programming time    min 20 us   max 40 us
 *   tERASE  Page (1kB) erase time    min 20 ms   max 40 ms
 *   tME     Mass erase time          min 20 ms   max 40 ms
 *
 * The Typ column is empty in that table: ST gives only a min and a max, so
 * every figure below is a range and not a typical. The part programs 16 bits
 * per operation (PM0075), so "word" here is one half-word and a 1 KB page is
 * 512 programming operations.
 *
 * Endurance is 1 kcycle min (DS5319 Rev 1; later revisions state 10 kcycles),
 * which is why anything rewritten per boot should clear bits instead of
 * erasing a page. */
#define F103_ERASE_MS_MIN  20.0
#define F103_ERASE_MS_MAX  40.0
#define F103_PROG_US_MIN   20.0
#define F103_PROG_US_MAX   40.0
#define F103_MHZ           72.0
#define F103_MD5_CYC_B     10.0
#define F103_SHA256_CYC_B  25.0

/* worst = 0 uses the datasheet minimums, worst = 1 the maximums. */
static inline double f103_flash_ms(long erases, size_t prog_bytes, int worst)
{
    double e = worst ? F103_ERASE_MS_MAX : F103_ERASE_MS_MIN;
    double p = worst ? F103_PROG_US_MAX  : F103_PROG_US_MIN;
    return (double)erases * e + (double)(prog_bytes / 2) * p / 1000.0;
}

/* One sector: erase the page, then program every half-word of it. */
static inline double f103_sector_ms(int worst)
{
    return f103_flash_ms(1, PAGE_SIZE, worst);
}

int  flash_create(const char *path);
int  flash_open(flash_t *f, const char *path);
void flash_close(flash_t *f);

void flash_erase_page(flash_t *f, int page);
/* Half-word granular, bits may only go 1 -> 0. Dies on violation. */
void flash_program(flash_t *f, size_t off, const void *buf, size_t len);
const uint8_t *flash_at(flash_t *f, size_t off);

void flash_sector_hash(flash_t *f, int index, uint8_t out[SECTOR_HASH_LEN]);

/* Meta bank ping-pong. Returns 0 if a valid bank was found. */
int  meta_load(flash_t *f, meta_t *m);
void meta_store(flash_t *f, meta_t *m);        /* bumps generation, erases the older bank */
void meta_init(meta_t *m);

/* A spare half-word in the journal page used as the "plan committed" flag,
 * cleared when the server sends COMMIT. Bit-clearing, so no erase. */
#define JRN_ARM_INDEX (SECTORS_SLOT + 2)

uint16_t journal_get(flash_t *f, int index);
void     journal_set(flash_t *f, int index, uint16_t state);
void     journal_clear(flash_t *f);            /* erases the journal page of the live bank */

identity_t *identity_get(flash_t *f);

#endif
