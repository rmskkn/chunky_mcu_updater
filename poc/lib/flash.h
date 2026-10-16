#ifndef CB_FLASH_H
#define CB_FLASH_H
#include <stdint.h>
#include <stddef.h>
#include "sha256.h"
#include "platform.h"

/* Flash geometry, as in DESIGN.md section 2 / 10.2 / 18. The numbers below
 * are a worked reference part's; every one of them is a build-time choice,
 * overridable for a different part, and none is assumed elsewhere in lib/.
 *
 * One application partition, updated in place. There is no second slot, so
 * there is nothing to fall back to: an update that fails to verify leaves the
 * device without a bootable image and the bootloader asks for another one.
 * That also means the running image cannot do the writing -- it would be
 * overwriting itself -- so the update belongs to the bootloader. */
/* Erase unit: what the part can erase. Sector: the delta unit (hashed,
 * diffed, sent, compressed). Both are build-time choices. */
#ifndef CB_ERASE_SIZE
#define CB_ERASE_SIZE  1024           /* minimum erasable unit of the part */
#endif
#ifndef CB_SECTOR_SIZE
#define CB_SECTOR_SIZE CB_ERASE_SIZE  /* delta unit */
#endif
#ifndef CB_FLASH_SIZE
#define CB_FLASH_SIZE  (128 * 1024)   /* total flash on the part */
#endif
#define ERASE_SIZE   CB_ERASE_SIZE
#define SECTOR_SIZE  CB_SECTOR_SIZE
#define FLASH_SIZE   CB_FLASH_SIZE

/* Whether a half-word may be programmed only once between erases, and
 * whether 0x0000 is exempt from that limit -- both properties of the part,
 * not a universal rule; this check can express "once or unlimited", not an
 * arbitrary count. The default below models one real part's flash (PM0075):
 * one program per half-word after an erase, except that 0x0000 may always
 * be written regardless of the half-word's current content. A part that
 * allows more than one program per word (so the limit does not apply), or
 * one with ECC and no zero exception, overrides these at build time;
 * flash_program() in flash.c enforces whatever is set. */
#ifndef CB_FLASH_PROGRAM_ONCE
#define CB_FLASH_PROGRAM_ONCE 1      /* 0 disables the limit entirely */
#endif
#ifndef CB_FLASH_ZERO_EXEMPT
#define CB_FLASH_ZERO_EXEMPT 1       /* 0x0000 does not count against the limit */
#endif

#define IS_POW2(x)   ((x) > 0 && ((x) & ((x) - 1)) == 0)
#define DIV_CEIL(a, b)   (((a) + (b) - 1) / (b))
#define ROUND_UP(a, b)   (DIV_CEIL(a, b) * (b))

_Static_assert(IS_POW2(ERASE_SIZE), "ERASE_SIZE must be a power of two");
_Static_assert(IS_POW2(SECTOR_SIZE), "SECTOR_SIZE must be a power of two");
/* A smaller sector would need read-modify-write of its neighbours in the same
 * unit, and break the per-sector journal's atomicity. */
_Static_assert(SECTOR_SIZE % ERASE_SIZE == 0,
               "a sector must be a whole number of erase units");
_Static_assert(SECTOR_SIZE <= 32768,
               "STEP_WRITE offsets are u16 and index the sector");
_Static_assert(FLASH_SIZE % ERASE_SIZE == 0, "flash is not a whole number of erase units");

/* Layout in erase units; only the application is rounded up to a sector. */
#define BL_UNITS         DIV_CEIL(15 * 1024, ERASE_SIZE)
#define ID_UNIT          BL_UNITS
#define META_HDR_UNITS   DIV_CEIL(4096, ERASE_SIZE)   /* meta_t area, 4 KB as before */
#define META_UNITS       (META_HDR_UNITS + 1)         /* + the journal's own unit */
#define META0_UNIT       (ID_UNIT + 1)
#define META1_UNIT       (META0_UNIT + META_UNITS)
#define APP_BASE         ((size_t)ROUND_UP((META1_UNIT + META_UNITS) * ERASE_SIZE, SECTOR_SIZE))
#define SECTORS_SLOT     ((int)((FLASH_SIZE - APP_BASE) / SECTOR_SIZE))
#define SLOT_SIZE        (SECTORS_SLOT * SECTOR_SIZE)
#define MANIFEST_ENTRIES SECTORS_SLOT

#if CB_ERASE_SIZE == 1024 && CB_SECTOR_SIZE == 1024
_Static_assert(BL_UNITS == 15 && ID_UNIT == 15 && META0_UNIT == 16 &&
               META1_UNIT == 21 && APP_BASE == 26 * 1024 && SECTORS_SLOT == 102,
               "the default geometry must keep the layout of DESIGN.md section 2");
#endif

/* Sector indices are partition-relative. A byte covers 256 sectors, which is
 * a 256 KB partition at a 1 KB sector and runs out on a 1 MB part, so the index
 * has its own type everywhere it appears: in the manifest, on the wire, and in
 * the plan the bootloader reads back. */
typedef uint16_t sector_idx_t;

/* Per-sector hashes are the full SHA-256 output (32 B), not truncated: they
 * answer one question -- does this sector already hold the bytes the
 * target needs -- and today are not a trust boundary, since image_sha256
 * and the release signature gate the result, so a collision costs a retried
 * update rather than a corrupt device. The full digest is kept (over a
 * truncation) so a per-sector hash can later be wrapped in HMAC-SHA256
 * without changing SECTOR_HASH_LEN or the wire format (DESIGN.md section
 * 10). Reusing SHA-256 also drops a second hash primitive from the
 * bootloader image. */
#define SECTOR_HASH_LEN SHA256_LEN

void sector_hash(const void *sector, uint8_t out[SECTOR_HASH_LEN]);

/* Three things grow with the number of sectors, and only one of them is the
 * index width. The asserts below turn each cliff into a build error rather
 * than silent corruption on a bigger part. */
_Static_assert(SECTORS_SLOT <= (1 << (8 * sizeof(sector_idx_t))) - 1,
               "sector index type too narrow for this partition size");

/* Journal states. With the default profile above (one program per erase,
 * 0x0000 exempt) a half-word that is never erased while a plan is
 * live has three states: erased, one arbitrary program, zero. A fourth
 * state between them (0xFFF0 -> 0xFF00) only clears bits and would still
 * violate the one-program limit; flash_program() rejects it. A part built
 * with CB_FLASH_ZERO_EXEMPT=0 (e.g. an ECC part with no such exception)
 * cannot reach JRN_DONE after JRN_VERIFIED this way and needs a journal
 * word per state instead -- the assert below catches that build. */
#define JRN_EMPTY     0xFFFFu
#define JRN_VERIFIED  0xFF00u
#define JRN_DONE      0x0000u
_Static_assert(!(CB_FLASH_PROGRAM_ONCE && !CB_FLASH_ZERO_EXEMPT),
               "this journal relies on 0x0000 being exempt from the "
               "one-program-per-erase limit; give it a word per state instead");

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

/* Bank layout: meta_t, then the journal in an erase unit of its own (it is
 * never erased while a plan is live, so it cannot share one). */
#define JOURNAL_OFF     (META_HDR_UNITS * ERASE_SIZE)

/* meta_t carries ~20 B per sector, so it grows with the partition. These
 * are the boundaries it has to respect: a 1 MB part at a 1 KB sector would
 * need 10 KB of plan and fits neither. */
_Static_assert(sizeof(meta_t) <= JOURNAL_OFF,
               "meta_t runs into the journal unit: use fewer, larger sectors, "
               "or give the bank more erase units");
_Static_assert((SECTORS_SLOT + 3) * 2 <= ERASE_SIZE,
               "the progress journal does not fit its erase unit");

/* last_result codes reported back in HELLO */
enum { RES_NONE = 0, RES_OK, RES_VERIFY_FAILED, RES_FLASH_FAIL };

typedef struct {
    plat_flash_t io;
    long     n_erase;      /* erase-unit erases, for the device time model */
    long     n_prog;
    size_t   prog_bytes;
    const char *path;
} flash_t;

/* Flash timing model, every figure a build-time default for the reference
 * part and overridable for another. Defaults below are datasheet DS5319
 * Table 23 "Flash memory characteristics", TA = -40 to +105 C:
 *
 *   tprog   Word programming time    min 20 us   max 40 us
 *   tERASE  Page (1kB) erase time    min 20 ms   max 40 ms
 *   tME     Mass erase time          min 20 ms   max 40 ms
 *
 * The Typ column is empty in that table: the datasheet gives only a min and
 * a max, so every figure below is a range and not a typical. That part
 * programs 16 bits per operation (PM0075), so "word" here is one half-word
 * and a 1 KB page is 512 programming operations.
 *
 * Endurance is 1 kcycle min (DS5319 Rev 1; later revisions state 10 kcycles),
 * which is why anything rewritten per boot should clear bits instead of
 * erasing a page. */
#ifndef CB_FLASH_ERASE_MS_MIN
#define CB_FLASH_ERASE_MS_MIN  20.0
#endif
#ifndef CB_FLASH_ERASE_MS_MAX
#define CB_FLASH_ERASE_MS_MAX  40.0
#endif
#ifndef CB_FLASH_PROG_US_MIN
#define CB_FLASH_PROG_US_MIN   20.0
#endif
#ifndef CB_FLASH_PROG_US_MAX
#define CB_FLASH_PROG_US_MAX   40.0
#endif
#ifndef CB_FLASH_MHZ
#define CB_FLASH_MHZ           72.0
#endif
#ifndef CB_FLASH_SHA256_CYC_B
#define CB_FLASH_SHA256_CYC_B  25.0
#endif

/* worst = 0 uses the minimums, worst = 1 the maximums. */
static inline double flash_timing_ms(long erases, size_t prog_bytes, int worst)
{
    double e = worst ? CB_FLASH_ERASE_MS_MAX : CB_FLASH_ERASE_MS_MIN;
    double p = worst ? CB_FLASH_PROG_US_MAX  : CB_FLASH_PROG_US_MIN;
    return (double)erases * e + (double)(prog_bytes / 2) * p / 1000.0;
}

/* One sector: erase its units, then program it. Assumes the erase time above
 * applies per unit regardless of unit size. */
static inline double flash_sector_timing_ms(int worst)
{
    return flash_timing_ms(SECTOR_SIZE / ERASE_SIZE, SECTOR_SIZE, worst);
}

int  flash_create(const char *path);
int  flash_open(flash_t *f, const char *path);
void flash_close(flash_t *f);

void flash_erase_unit(flash_t *f, int unit);
/* Half-word granular, bits may only go 1 -> 0. Dies on violation. */
void flash_program(flash_t *f, size_t off, const void *buf, size_t len);
const uint8_t *flash_at(flash_t *f, size_t off);

void flash_sector_hash(flash_t *f, int index, uint8_t out[SECTOR_HASH_LEN]);

/* Erase, then program, one sector of the application partition in place. */
void flash_write_app_sector(flash_t *f, sector_idx_t index, const uint8_t data[SECTOR_SIZE]);

/* Meta bank ping-pong. Returns 0 if a valid bank was found. */
int  meta_load(flash_t *f, meta_t *m);
void meta_store(flash_t *f, meta_t *m);        /* bumps generation, erases the older bank */
void meta_init(meta_t *m);

/* A spare half-word in the journal unit used as the "plan committed" flag,
 * cleared when the server sends COMMIT. Bit-clearing, so no erase. */
#define JRN_ARM_INDEX (SECTORS_SLOT + 2)

uint16_t journal_get(flash_t *f, int index);
void     journal_set(flash_t *f, int index, uint16_t state);
void     journal_clear(flash_t *f);            /* erases the journal unit of the live bank */

identity_t *identity_get(flash_t *f);

#endif
