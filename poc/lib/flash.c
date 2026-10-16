#include "flash.h"
#include "util.h"
#include <string.h>

/* A bank is meta_t in META_HDR_UNITS units, then the journal's own unit. */
#define BANK_UNIT(b)    ((b) == 0 ? META0_UNIT : META1_UNIT)
#define BANK_META(b)    ((size_t)BANK_UNIT(b) * ERASE_SIZE)
#define BANK_JOURNAL(b) (BANK_META(b) + JOURNAL_OFF)

static int live_bank;   /* bank meta_load() picked, or the one meta_store() wrote */

int flash_create(const char *path)
{
    int rc = plat_flash_create(path, FLASH_SIZE, 0xFF);
    if (rc) return rc;

    flash_t f;
    if (plat_flash_open(&f.io, path, FLASH_SIZE)) return -1;

    /* a recognisable bootloader region so the image is visible in a hex
     * dump; 0xFF -> 0xEE only clears bits, same as a real program would. */
    uint8_t bl_marker[BL_UNITS * ERASE_SIZE];
    memset(bl_marker, 0xEE, sizeof bl_marker);
    plat_flash_write(&f.io, 0, bl_marker, sizeof bl_marker);

    identity_t id;
    memset(&id, 0xFF, sizeof id);
    id.magic = ID_MAGIC;
    rnd_bytes(id.device_id, sizeof id.device_id);
    /* fixed keys: the PoC server shares them, standing in for a provisioned
     * device key and the root public key held in the bootloader region. */
    for (int i = 0; i < 32; i++) { id.dev_key[i] = (uint8_t)(0xA0 + i); id.root_key[i] = (uint8_t)(0x50 + i); }
    plat_flash_write(&f.io, (size_t)ID_UNIT * ERASE_SIZE, &id, sizeof id);

    plat_flash_close(&f.io);
    return 0;
}

int flash_open(flash_t *f, const char *path)
{
    if (plat_flash_open(&f->io, path, FLASH_SIZE)) return -1;
    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
    f->path = path;
    live_bank = -1;
    return 0;
}

void flash_close(flash_t *f)
{
    plat_flash_close(&f->io);
}

void flash_erase_unit(flash_t *f, int unit)
{
    plat_flash_erase(&f->io, (size_t)unit * ERASE_SIZE, ERASE_SIZE);
    f->n_erase++;
}

void flash_program(flash_t *f, size_t off, const void *buf, size_t len)
{
    const uint8_t *src = buf;

    if (off % 2 || len % 2)
        die("flash: program at 0x%zx len %zu is not half-word aligned", off, len);
    if (off + len > FLASH_SIZE)
        die("flash: program past end of flash");

    /* Two rules, both per half-word, both configured in flash.h for the
     * part this build targets. Bits may only go 1 -> 0, always. Whether a
     * half-word may be programmed only once after an erase, and whether
     * 0x0000 is exempt from that, are build-time choices
     * (CB_FLASH_PROGRAM_ONCE, CB_FLASH_ZERO_EXEMPT), not hardcoded here: on
     * a part with that limit, clearing bits in two steps (0xFFF0 -> 0xFF00)
     * obeys the first rule and still reprograms an already-programmed
     * half-word, so modelling only the first would let a journal encoding
     * pass here and fail on that device; a part built with the limit off
     * does not need or get that second check. */
    const uint8_t *cur = plat_flash_read(&f->io, off);
    for (size_t i = 0; i < len; i += 2) {
        uint16_t c, s;
        memcpy(&c, cur + i, 2);
        memcpy(&s, src + i, 2);
        if ((c & s) != s)
            die("flash: program at 0x%zx would set bits (0x%04x -> 0x%04x); "
                "erase the unit first", off + i, c, s);
#if CB_FLASH_PROGRAM_ONCE
        if (c != 0xFFFFu && !(CB_FLASH_ZERO_EXEMPT && s == 0x0000u))
            die("flash: program at 0x%zx reprograms a programmed half-word "
                "(0x%04x -> 0x%04x); this part allows one program per erase",
                off + i, c, s);
#endif
    }
    plat_flash_write(&f->io, off, src, len);
    f->n_prog++;
    f->prog_bytes += len;
}

const uint8_t *flash_at(flash_t *f, size_t off) { return plat_flash_read(&f->io, off); }

void flash_write_app_sector(flash_t *f, sector_idx_t index, const uint8_t data[SECTOR_SIZE])
{
    size_t off = APP_BASE + (size_t)index * SECTOR_SIZE;

    for (int u = 0; u < SECTOR_SIZE / ERASE_SIZE; u++)
        flash_erase_unit(f, (int)(off / ERASE_SIZE) + u);
    flash_program(f, off, data, SECTOR_SIZE);
}

void sector_hash(const void *sector, uint8_t out[SECTOR_HASH_LEN])
{
    sha128(sector, SECTOR_SIZE, out);
}

void flash_sector_hash(flash_t *f, int index, uint8_t out[SECTOR_HASH_LEN])
{
    sector_hash(flash_at(f, APP_BASE + (size_t)index * SECTOR_SIZE), out);
}

identity_t *identity_get(flash_t *f)
{
    identity_t *id = (identity_t *)flash_at(f, (size_t)ID_UNIT * ERASE_SIZE);
    if (id->magic != ID_MAGIC)
        die("device identity unit is blank: provisioning fault");
    return id;
}

void meta_init(meta_t *m)
{
    memset(m, 0, sizeof *m);
    m->magic        = META_MAGIC;
    m->generation   = 0;
    m->plan_state   = PLAN_NONE;
}

static int bank_valid(flash_t *f, int bank, meta_t *out)
{
    meta_t m;
    memcpy(&m, flash_at(f, BANK_META(bank)), sizeof m);
    if (m.magic != META_MAGIC) return 0;
    uint32_t want = m.crc;
    m.crc = 0;
    if (crc32_buf(&m, sizeof m) != want) return 0;
    if (out) { memcpy(out, &m, sizeof m); out->crc = want; }
    return 1;
}

int meta_load(flash_t *f, meta_t *m)
{
    meta_t a, b;
    int ha = bank_valid(f, 0, &a);
    int hb = bank_valid(f, 1, &b);

    if (!ha && !hb) { meta_init(m); live_bank = 1; return -1; }
    if (ha && (!hb || a.generation >= b.generation)) { *m = a; live_bank = 0; return 0; }
    *m = b; live_bank = 1;
    return 0;
}

void meta_store(flash_t *f, meta_t *m)
{
    int target = (live_bank == 0) ? 1 : 0;   /* write the other bank */

    m->magic = META_MAGIC;
    m->generation++;
    m->crc = 0;
    m->crc = crc32_buf(m, sizeof *m);

    for (int u = 0; u < META_UNITS; u++) flash_erase_unit(f, BANK_UNIT(target) + u);
    flash_program(f, BANK_META(target), m, sizeof *m);
    live_bank = target;
}

uint16_t journal_get(flash_t *f, int index)
{
    uint16_t v;
    memcpy(&v, flash_at(f, BANK_JOURNAL(live_bank) + (size_t)index * 2), 2);
    return v;
}

void journal_set(flash_t *f, int index, uint16_t state)
{
    flash_program(f, BANK_JOURNAL(live_bank) + (size_t)index * 2, &state, 2);
}

void journal_clear(flash_t *f)
{
    flash_erase_unit(f, BANK_UNIT(live_bank) + META_HDR_UNITS);
}
