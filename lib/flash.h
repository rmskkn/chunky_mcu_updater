#ifndef CB_FLASH_H
#define CB_FLASH_H
#include <stdint.h>
#include <stddef.h>
#include "sha256.h"
#include "flash_port.h"
#include "protocol.h"
#include "util.h"

/* Flash geometry and the part's flash rules come from the device catalog:
 * device/<DEVICE>/device.toml, which tools/gen_device.py turns into a
 * generated geometry.h at build time (make DEVICE=...). lib/ names no part;
 * it checks below that whatever the device file sets is a layout this code
 * can run on, without trusting the generator to have checked it.
 *
 * The device file lays out two equal application slots, A and B (DESIGN.md
 * section 2). The update logic here is still the single-partition variant
 * (section 18): it updates slot A in place, and slot B is reserved for the
 * A/B flow. With one live slot there is nothing to fall back to: an update
 * that fails to verify leaves the device without a bootable image and the
 * bootloader asks for another one. That also means the running image cannot
 * do the writing -- it would be overwriting itself -- so the update belongs
 * to the bootloader. */
#include "geometry.h"

#define IS_POW2(x)   ((x) > 0 && ((x) & ((x) - 1)) == 0)

_Static_assert(IS_POW2(ERASE_SIZE), "ERASE_SIZE must be a power of two");
_Static_assert(IS_POW2(SECTOR_SIZE), "SECTOR_SIZE must be a power of two");
/* A smaller sector would need read-modify-write of its neighbours in the same
 * unit, and break the per-sector journal's atomicity. */
_Static_assert(SECTOR_SIZE % ERASE_SIZE == 0,
               "a sector must be a whole number of erase units");
_Static_assert(SECTOR_SIZE <= 32768,
               "STEP_WRITE offsets are u16 and index the sector");
_Static_assert(FLASH_SIZE % ERASE_SIZE == 0, "flash is not a whole number of erase units");

/* The device file's regions, in order and without overlap: bootloader,
 * identity, two meta banks, slot A, slot B. */
_Static_assert(ID_UNIT >= BL_UNITS, "identity overlaps the bootloader");
_Static_assert(META0_UNIT > ID_UNIT, "meta bank 0 overlaps the identity unit");
_Static_assert(META1_UNIT >= META0_UNIT + META_UNITS, "meta banks overlap");
_Static_assert(SLOT_A_BASE >= (size_t)(META1_UNIT + META_UNITS) * ERASE_SIZE,
               "slot A overlaps meta bank 1");
_Static_assert(SLOT_A_BASE % SECTOR_SIZE == 0 && SLOT_B_BASE % SECTOR_SIZE == 0,
               "slots must start on a sector boundary");
_Static_assert(SLOT_SIZE > 0 && SLOT_SIZE % SECTOR_SIZE == 0,
               "a slot must be a whole, non-zero number of sectors");
_Static_assert(SLOT_B_BASE >= SLOT_A_BASE + SLOT_SIZE, "slot B overlaps slot A");
_Static_assert(SLOT_B_BASE + SLOT_SIZE <= FLASH_SIZE, "slot B runs past the end of flash");

/* The partition the update writes, in place. */
#define APP_BASE         SLOT_A_BASE
#define SECTORS_SLOT     ((int)(SLOT_SIZE / SECTOR_SIZE))
#define MANIFEST_ENTRIES SECTORS_SLOT

/* Callers that walk a large region of flash (a sector, the slot) read it in
 * pieces of this size, so none of them needs a buffer the size of the
 * region. */
#define FLASH_READ_CHUNK 256
_Static_assert(SECTOR_SIZE % FLASH_READ_CHUNK == 0,
               "a sector must be a whole number of FLASH_READ_CHUNK reads");
_Static_assert(SECTOR_SIZE % WRITE_CHUNK == 0,
               "a sector must be a whole number of STEP_WRITE fragments");

/* sector_idx_t and SECTOR_HASH_LEN are wire types: see protocol.h;
 * sector_hash() is in manifest.h. */

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

typedef struct {
    plat_flash_t *io;      /* the port this model drives; not owned */
    long     n_erase;      /* erase-unit erases, for the device time model */
    long     n_prog;
    size_t   prog_bytes;
    cb_err_t err;          /* why the last flash call that returned -1 failed */
} flash_t;

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

/* Binds a part the caller has already opened and resets the counters. */
void flash_init(flash_t *f, plat_flash_t *io);

/* Every call below that returns int gives 0, or -1 with the reason in
 * f->err. A -1 from the program rules is the part refusing the write (on
 * silicon: PGERR), not a reason to stop the device. */
int  flash_erase_unit(flash_t *f, int unit);
/* Half-word granular, bits may only go 1 -> 0. */
int  flash_program(flash_t *f, size_t off, const void *buf, size_t len);
/* Copies len bytes at off into buf. Out of range, buf reads as erased
 * (0xFF), so a hash over it mismatches and the caller's check fails. */
int  flash_read(flash_t *f, size_t off, void *buf, size_t len);

void flash_sector_hash(flash_t *f, int index, uint8_t out[SECTOR_HASH_LEN]);

/* Erase, then program, one sector of the application partition in place. */
int  flash_write_app_sector(flash_t *f, sector_idx_t index, const uint8_t data[SECTOR_SIZE]);

/* Meta bank ping-pong. Returns 0 if a valid bank was found. */
int  meta_load(flash_t *f, meta_t *m);
int  meta_store(flash_t *f, meta_t *m);        /* bumps generation, erases the older bank */
void meta_init(meta_t *m);

/* A spare half-word in the journal unit used as the "plan committed" flag,
 * cleared when the server sends COMMIT. Bit-clearing, so no erase. */
#define JRN_ARM_INDEX (SECTORS_SLOT + 2)

uint16_t journal_get(flash_t *f, int index);
int      journal_set(flash_t *f, int index, uint16_t state);
int      journal_clear(flash_t *f);            /* erases the journal unit of the live bank */

/* Copies the identity unit into *out. -1 if it is blank (a provisioning
 * fault). */
int  identity_get(flash_t *f, identity_t *out);

#endif
