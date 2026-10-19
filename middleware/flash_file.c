#include "flash_file.h"
#include "image.h"
#include "host.h"
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* This backend's flash port (lib/flash_port.h): the part is a file the size
 * of FLASH_SIZE, mmap'd, and every write or erase is synced to disk at once
 * so a killed process leaves exactly what a power cut would. */
struct plat_flash {
    uint8_t *base;
    int      fd;
};

static void sync_range(plat_flash_t *p, size_t off, size_t len)
{
    size_t start = off & ~(size_t)4095;
    msync(p->base + start, len + (off - start), MS_SYNC);
}

void plat_flash_read(plat_flash_t *p, size_t off, void *buf, size_t len)
{
    memcpy(buf, p->base + off, len);
}

void plat_flash_write(plat_flash_t *p, size_t off, const void *buf, size_t len)
{
    memcpy(p->base + off, buf, len);
    sync_range(p, off, len);
}

void plat_flash_erase(plat_flash_t *p, size_t off, size_t len)
{
    memset(p->base + off, 0xFF, len);
    sync_range(p, off, len);
}

int flash_file_create(const char *path)
{
    /* One-shot image of the whole part, sized to the one geometry this
     * build targets. */
    static uint8_t part[FLASH_SIZE];
    memset(part, 0xFF, sizeof part);

    /* a recognisable bootloader region so the image is visible in a hex
     * dump; 0xFF -> 0xEE only clears bits, same as a real program would. */
    memset(part, 0xEE, (size_t)BL_UNITS * ERASE_SIZE);

    identity_t id;
    memset(&id, 0xFF, sizeof id);
    id.magic = ID_MAGIC;
    rnd_bytes(id.device_id, sizeof id.device_id);
    /* fixed keys: the PoC server shares them, standing in for a provisioned
     * device key and the root public key held in the bootloader region. */
    for (int i = 0; i < 32; i++) { id.dev_key[i] = (uint8_t)(0xA0 + i); id.root_key[i] = (uint8_t)(0x50 + i); }
    memcpy(part + (size_t)ID_UNIT * ERASE_SIZE, &id, sizeof id);

    return write_file(path, part, sizeof part);
}

int flash_file_open(flash_t *f, const char *path)
{
    plat_flash_t *p = malloc(sizeof *p);
    if (!p) return -1;
    p->fd = open(path, O_RDWR);
    if (p->fd < 0) { int e = errno; free(p); errno = e; return -1; }
    p->base = mmap(NULL, FLASH_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, p->fd, 0);
    if (p->base == MAP_FAILED) {
        int e = errno;
        close(p->fd);
        free(p);
        errno = e;
        return -1;
    }
    flash_init(f, p);
    return 0;
}

void flash_file_close(flash_t *f)
{
    plat_flash_t *p = f->io;
    msync(p->base, FLASH_SIZE, MS_SYNC);
    munmap(p->base, FLASH_SIZE);
    close(p->fd);
    free(p);
    f->io = NULL;
}

/* Writes a raw firmware binary into the application partition as a signed
 * image: what a factory programmer or a debugger would leave behind. A host
 * tool's job, so it lives here and not in lib/. -1 with the reason in
 * f->err. */
static int install_image(flash_t *f, const uint8_t *bin, size_t binlen,
                  uint32_t version, const uint8_t root_key[32])
{
    uint8_t *img = malloc(SLOT_SIZE);
    meta_t m;

    if (!img) return err_set(&f->err, "out of memory for a %d B slot image", SLOT_SIZE);
    if (image_build(bin, binlen, version, root_key, img) < 0) {
        free(img);
        return err_set(&f->err, "image is %zu B, over the %d B slot payload",
                       binlen, IMG_MAX_PAYLOAD);
    }

    for (int i = 0; i < SECTORS_SLOT; i++)
        if (flash_write_app_sector(f, (sector_idx_t)i, img + (size_t)i * SECTOR_SIZE) < 0) {
            free(img);
            return -1;
        }
    free(img);

    meta_load(f, &m);
    if (m.magic != META_MAGIC) meta_init(&m);
    m.plan_state  = PLAN_COMMITTED;
    m.last_result = RES_NONE;
    return meta_store(f, &m);
}


/* Opens the device, creating it on first use with the firmware it shipped
 * with in slot 0. The manifest the client sends is a statement about real
 * flash contents, so the device has to have some. */
int device_open_or_create(flash_t *f, const char *flash_path,
                          const char *initial_path, const uint8_t root_key[32],
                          log_fn log, void *log_ctx, cb_err_t *err)
{
    if (flash_file_open(f, flash_path) == 0) return 0;
    if (errno != ENOENT)
        return err_set(err, "cannot open %s: %s", flash_path, strerror(errno));
    if (!initial_path)
        return err_set(err, "%s does not exist\n"
                       "  pass --initial FILE: the firmware the device shipped with,\n"
                       "  which is what its first manifest describes", flash_path);

    size_t binlen;
    uint8_t *bin = read_file(initial_path, &binlen);
    if (!bin) return err_set(err, "cannot read %s", initial_path);
    if (flash_file_create(flash_path) < 0) {
        free(bin);
        return err_set(err, "cannot create %s", flash_path);
    }
    /* Seed with the fault injection and the counters off: this is the factory
     * programmer's work, not the update's, and it must not show up in either
     * the timings or --fail-after. */
    if (flash_file_open(f, flash_path) < 0) {
        free(bin);
        return err_set(err, "cannot open %s: %s", flash_path, strerror(errno));
    }

    int rc = install_image(f, bin, binlen, 1, root_key);
    free(bin);
    if (rc < 0) {
        err_set(err, "%s: %s", initial_path, f->err.msg);
        flash_file_close(f);
        return -1;
    }
    log_fmt(log, log_ctx, "device: created %s with %s as version 1", flash_path, initial_path);

    f->n_erase = f->n_prog = 0;
    f->prog_bytes = 0;
    return 0;
}
