#include "flash.h"
#include "util.h"
#include <string.h>

/* A bank is meta_t in META_HDR_UNITS units, then the journal's own unit. */
#define BANK_UNIT(b)    ((b) == 0 ? META0_UNIT : META1_UNIT)
#define BANK_META(b)    ((size_t)BANK_UNIT(b) * ERASE_SIZE)
#define BANK_JOURNAL(b) (BANK_META(b) + JOURNAL_OFF)

static int live_bank;   /* bank meta_load() picked, or the one meta_store() wrote */

void flash_init(flash_t *f, plat_flash_t *io)
{
    f->io = io;
    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
    live_bank = -1;
}

int flash_erase_unit(flash_t *f, int unit)
{
    if (unit < 0 || unit >= FLASH_SIZE / ERASE_SIZE)
        return err_set(&f->err, "flash: erase of unit %d, past end of flash", unit);
    plat_flash_erase(f->io, (size_t)unit * ERASE_SIZE, ERASE_SIZE);
    f->n_erase++;
    return 0;
}

int flash_program(flash_t *f, size_t off, const void *buf, size_t len)
{
    const uint8_t *src = buf;

    if (off % 2 || len % 2)
        return err_set(&f->err, "flash: program at 0x%zx len %zu is not half-word aligned",
                       off, len);
    if (off > FLASH_SIZE || len > FLASH_SIZE - off)
        return err_set(&f->err, "flash: program at 0x%zx len %zu past end of flash", off, len);

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
    for (size_t i = 0; i < len; i += 2) {
        uint16_t c, s;
        flash_read(f, off + i, &c, 2);
        memcpy(&s, src + i, 2);
        /* Checked before anything is written, so a refused program leaves
         * flash as it was. */
        if ((c & s) != s)
            return err_set(&f->err, "flash: program at 0x%zx would set bits "
                           "(0x%04x -> 0x%04x); erase the unit first", off + i, c, s);
#if CB_FLASH_PROGRAM_ONCE
        if (c != 0xFFFFu && !(CB_FLASH_ZERO_EXEMPT && s == 0x0000u))
            return err_set(&f->err, "flash: program at 0x%zx reprograms a programmed "
                           "half-word (0x%04x -> 0x%04x); this part allows one program "
                           "per erase", off + i, c, s);
#endif
    }
    plat_flash_write(f->io, off, src, len);
    f->n_prog++;
    f->prog_bytes += len;
    return 0;
}

int flash_read(flash_t *f, size_t off, void *buf, size_t len)
{
    if (off > FLASH_SIZE || len > FLASH_SIZE - off) {
        memset(buf, 0xFF, len);
        return err_set(&f->err, "flash: read at 0x%zx len %zu past end of flash", off, len);
    }
    plat_flash_read(f->io, off, buf, len);
    return 0;
}

int flash_write_app_sector(flash_t *f, sector_idx_t index, const uint8_t data[SECTOR_SIZE])
{
    if (index >= SECTORS_SLOT)
        return err_set(&f->err, "flash: sector %u outside the partition", index);
    size_t off = APP_BASE + (size_t)index * SECTOR_SIZE;

    for (int u = 0; u < SECTOR_SIZE / ERASE_SIZE; u++)
        if (flash_erase_unit(f, (int)(off / ERASE_SIZE) + u) < 0) return -1;
    return flash_program(f, off, data, SECTOR_SIZE);
}


void flash_sector_hash(flash_t *f, int index, uint8_t out[SECTOR_HASH_LEN])
{
    size_t base = APP_BASE + (size_t)index * SECTOR_SIZE;
    uint8_t chunk[FLASH_READ_CHUNK];
    sha256_t s;

    sha256_init(&s);
    for (size_t o = 0; o < SECTOR_SIZE; o += sizeof chunk) {
        flash_read(f, base + o, chunk, sizeof chunk);
        sha256_update(&s, chunk, sizeof chunk);
    }
    uint8_t full[SHA256_LEN];
    sha256_final(&s, full);
    memcpy(out, full, SECTOR_HASH_LEN);
}

int identity_get(flash_t *f, identity_t *out)
{
    if (flash_read(f, (size_t)ID_UNIT * ERASE_SIZE, out, sizeof *out) < 0) return -1;
    if (out->magic != ID_MAGIC)
        return err_set(&f->err, "device identity unit is blank: provisioning fault");
    return 0;
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
    flash_read(f, BANK_META(bank), &m, sizeof m);
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

int meta_store(flash_t *f, meta_t *m)
{
    int target = (live_bank == 0) ? 1 : 0;   /* write the other bank */

    m->magic = META_MAGIC;
    m->generation++;
    m->crc = 0;
    m->crc = crc32_buf(m, sizeof *m);

    /* On a failure the live bank stays the older one, which is still whole:
     * that is what the ping-pong is for. */
    for (int u = 0; u < META_UNITS; u++)
        if (flash_erase_unit(f, BANK_UNIT(target) + u) < 0) return -1;
    if (flash_program(f, BANK_META(target), m, sizeof *m) < 0) return -1;
    live_bank = target;
    return 0;
}

uint16_t journal_get(flash_t *f, int index)
{
    uint16_t v;
    flash_read(f, BANK_JOURNAL(live_bank) + (size_t)index * 2, &v, 2);
    return v;
}

int journal_set(flash_t *f, int index, uint16_t state)
{
    return flash_program(f, BANK_JOURNAL(live_bank) + (size_t)index * 2, &state, 2);
}

int journal_clear(flash_t *f)
{
    return flash_erase_unit(f, BANK_UNIT(live_bank) + META_HDR_UNITS);
}
