#ifndef CB_FLASH_H
#define CB_FLASH_H
#include <stdint.h>
#include <stddef.h>
#include "md5.h"
#include "sha256.h"

/* Flash geometry, STM32F103CB as in DESIGN.md section 2 / 10.2. */
#define PAGE_SIZE     1024
#define PAGE_COUNT    128
#define FLASH_SIZE    (PAGE_SIZE * PAGE_COUNT)

#define BL_PAGE       0
#define BL_PAGES      15
#define ID_PAGE       15
#define META0_PAGE    16
#define META1_PAGE    20
#define META_PAGES    4
#define SLOT0_PAGE    24
#define SLOT1_PAGE    76
#define SLOT_PAGES    52

#define SLOT_SIZE     (SLOT_PAGES * PAGE_SIZE)
#define SECTORS_SLOT  SLOT_PAGES
#define SLOT_COUNT    2
#define MANIFEST_ENTRIES (SLOT_COUNT * SECTORS_SLOT)

#define SLOT_BASE(s)  (((s) == 0 ? SLOT0_PAGE : SLOT1_PAGE) * PAGE_SIZE)

/* Journal states. Every transition only clears bits, which is what an
 * STM32F103 half-word program can do without erasing the page first. */
#define JRN_EMPTY     0xFFFFu
#define JRN_WRITTEN   0xFFF0u
#define JRN_VERIFIED  0xFF00u
#define JRN_DONE      0x0000u

/* plan_state */
enum { PLAN_NONE = 0, PLAN_STAGING, PLAN_READY, PLAN_APPLYING,
       PLAN_TRIAL, PLAN_COMMITTED };

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
    uint8_t  active_slot;
    uint8_t  pending_slot;   /* 0xFF = none */
    uint8_t  plan_state;
    uint8_t  try_count;
    uint32_t target_version;
    uint8_t  target_sha256[SHA256_LEN];
    uint8_t  target_mac[SHA256_LEN];
    uint16_t step_count;
    uint8_t  op[SECTORS_SLOT];
    uint8_t  src_slot[SECTORS_SLOT];
    uint8_t  src_index[SECTORS_SLOT];
    uint8_t  target_md5[SECTORS_SLOT][MD5_LEN];
    uint32_t last_result;
} meta_t;

#define MAX_TRIES 3

/* last_result codes reported back in HELLO */
enum { RES_NONE = 0, RES_OK, RES_UNROLLED, RES_TRIAL_FAILED, RES_FLASH_FAIL };

typedef struct {
    uint8_t *map;
    int      fd;
    int      fail_after;   /* -1 = never; else _exit after N programs */
    long     programs;
    const char *path;
} flash_t;

int  flash_create(const char *path);
int  flash_open(flash_t *f, const char *path, int fail_after);
void flash_close(flash_t *f);

void flash_erase_page(flash_t *f, int page);
/* Half-word granular, bits may only go 1 -> 0. Dies on violation. */
void flash_program(flash_t *f, size_t off, const void *buf, size_t len);
const uint8_t *flash_at(flash_t *f, size_t off);

void flash_sector_md5(flash_t *f, int slot, int index, uint8_t out[MD5_LEN]);

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
