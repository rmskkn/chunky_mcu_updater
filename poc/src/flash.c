#include "flash.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

/* Bank 0 holds meta at page META0_PAGE and its journal at META0_PAGE+2. */
#define BANK_PAGE(b)    ((b) == 0 ? META0_PAGE : META1_PAGE)
#define BANK_META(b)    ((size_t)BANK_PAGE(b) * PAGE_SIZE)
#define BANK_JOURNAL(b) (BANK_META(b) + JOURNAL_OFF)

static int live_bank;   /* bank meta_load() picked, or the one meta_store() wrote */

int flash_create(const char *path)
{
    uint8_t *buf = malloc(FLASH_SIZE);
    memset(buf, 0xFF, FLASH_SIZE);
    /* a recognisable bootloader region so the map is visible in a hex dump */
    memset(buf, 0xEE, BL_PAGES * PAGE_SIZE);

    identity_t id;
    memset(&id, 0xFF, sizeof id);
    id.magic = ID_MAGIC;
    rnd_bytes(id.device_id, sizeof id.device_id);
    /* fixed keys: the PoC server shares them, standing in for a provisioned
     * device key and the root public key held in the bootloader region. */
    for (int i = 0; i < 32; i++) { id.dev_key[i] = (uint8_t)(0xA0 + i); id.root_key[i] = (uint8_t)(0x50 + i); }
    memcpy(buf + (size_t)ID_PAGE * PAGE_SIZE, &id, sizeof id);

    int rc = write_file(path, buf, FLASH_SIZE);
    free(buf);
    return rc;
}

int flash_open(flash_t *f, const char *path)
{
    f->fd = open(path, O_RDWR);
    if (f->fd < 0) return -1;
    f->map = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, f->fd, 0);
    if (f->map == MAP_FAILED) { close(f->fd); return -1; }
    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
    f->path = path;
    live_bank = -1;
    return 0;
}

void flash_close(flash_t *f)
{
    msync(f->map, FLASH_SIZE, MS_SYNC);
    munmap(f->map, FLASH_SIZE);
    close(f->fd);
}

static void sync_range(flash_t *f, size_t off, size_t len)
{
    size_t start = off & ~(size_t)4095;
    msync(f->map + start, len + (off - start), MS_SYNC);
}

void flash_erase_page(flash_t *f, int page)
{
    size_t off = (size_t)page * PAGE_SIZE;
    memset(f->map + off, 0xFF, PAGE_SIZE);
    sync_range(f, off, PAGE_SIZE);
    f->n_erase++;
}

void flash_program(flash_t *f, size_t off, const void *buf, size_t len)
{
    const uint8_t *src = buf;

    if (off % 2 || len % 2)
        die("flash: program at 0x%zx len %zu is not half-word aligned", off, len);
    if (off + len > FLASH_SIZE)
        die("flash: program past end of flash");

    for (size_t i = 0; i < len; i++) {
        uint8_t old = f->map[off + i];
        if ((old & src[i]) != src[i])
            die("flash: program at 0x%zx would set bits (0x%02x -> 0x%02x); "
                "erase the page first", off + i, old, src[i]);
    }
    memcpy(f->map + off, src, len);
    sync_range(f, off, len);
    f->n_prog++;
    f->prog_bytes += len;
}

const uint8_t *flash_at(flash_t *f, size_t off) { return f->map + off; }

void flash_sector_md5(flash_t *f, int index, uint8_t out[MD5_LEN])
{
    md5(f->map + APP_BASE + (size_t)index * PAGE_SIZE, PAGE_SIZE, out);
}

identity_t *identity_get(flash_t *f)
{
    identity_t *id = (identity_t *)(f->map + (size_t)ID_PAGE * PAGE_SIZE);
    if (id->magic != ID_MAGIC)
        die("device identity page is blank: provisioning fault");
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
    memcpy(&m, f->map + BANK_META(bank), sizeof m);
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

    for (int p = 0; p < META_PAGES; p++) flash_erase_page(f, BANK_PAGE(target) + p);
    flash_program(f, BANK_META(target), m, sizeof *m);
    live_bank = target;
}

uint16_t journal_get(flash_t *f, int index)
{
    uint16_t v;
    memcpy(&v, f->map + BANK_JOURNAL(live_bank) + (size_t)index * 2, 2);
    return v;
}

void journal_set(flash_t *f, int index, uint16_t state)
{
    flash_program(f, BANK_JOURNAL(live_bank) + (size_t)index * 2, &state, 2);
}

void journal_clear(flash_t *f)
{
    flash_erase_page(f, BANK_PAGE(live_bank) + META_PAGES_HDR);
}
