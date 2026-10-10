/* cb-flash: provisioning and inspection tool for the emulated flash, plus the
 * image helpers the scripts use to build test releases. */
#include "flash.h"
#include "image.h"
#include "manifest.h"
#include "keys.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  cb-flash init FLASH\n"
        "  cb-flash provision FLASH SLOT BIN VERSION\n"
        "  cb-flash manifest FLASH\n"
        "  cb-flash map FLASH\n"
        "  cb-flash mkimage BIN VERSION OUT\n"
        "  cb-flash mutate IN OUT RUNS SEED     patch RUNS 16-byte runs\n"
        "  cb-flash shift IN OUT NBYTES         insert NBYTES near the start\n");
    return 2;
}

static int cmd_init(const char *path)
{
    if (flash_create(path) < 0) die("cannot write %s", path);
    printf("created %s, %d B (%d pages of %d B)\n", path, FLASH_SIZE, PAGE_COUNT, PAGE_SIZE);
    return 0;
}

static int cmd_provision(const char *path, int slot, const char *binpath, uint32_t ver)
{
    flash_t f;
    size_t binlen;
    uint8_t root_key[32], *img;

    poc_root_key(root_key);
    uint8_t *bin = read_file(binpath, &binlen);
    if (!bin) die("cannot read %s", binpath);
    img = malloc(SLOT_SIZE);
    if (image_build(bin, binlen, ver, root_key, img) < 0)
        die("%s is %zu B, over the %d B slot payload", binpath, binlen, IMG_MAX_PAYLOAD);
    free(bin);

    if (flash_open(&f, path, -1) < 0) die("cannot open %s", path);
    int base = slot == 0 ? SLOT0_PAGE : SLOT1_PAGE;
    for (int i = 0; i < SLOT_PAGES; i++) {
        flash_erase_page(&f, base + i);
        flash_program(&f, (size_t)(base + i) * PAGE_SIZE, img + (size_t)i * PAGE_SIZE, PAGE_SIZE);
    }
    meta_t m;
    meta_load(&f, &m);
    if (m.magic != META_MAGIC) meta_init(&m);
    m.active_slot  = (uint8_t)slot;
    m.pending_slot = 0xFF;
    m.plan_state   = PLAN_COMMITTED;
    m.last_result  = RES_NONE;
    meta_store(&f, &m);
    flash_close(&f);
    free(img);
    printf("provisioned slot %d with %s as version %u, active\n", slot, binpath, ver);
    return 0;
}

static int cmd_manifest(const char *path)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN];
    char hex[80];

    if (flash_open(&f, path, -1) < 0) die("cannot open %s", path);
    meta_load(&f, &m);
    manifest_build(&f, m.active_slot, &man);
    for (int k = 0; k < MANIFEST_ENTRIES; k++) {
        hexdump_line(man.e[k].md5, 6, hex);
        printf("slot %u sector %2u  %s%s", man.e[k].slot, man.e[k].index, hex,
               (k % 4 == 3) ? "\n" : "   ");
    }
    if (MANIFEST_ENTRIES % 4) printf("\n");
    manifest_digest(&man, d);
    hexdump_line(d, 32, hex);
    printf("body %zu B  digest %s\n", MANIFEST_BODY_LEN, hex);
    flash_close(&f);
    return 0;
}

static int cmd_map(const char *path)
{
    flash_t f;
    if (flash_open(&f, path, -1) < 0) die("cannot open %s", path);
    printf("region            pages      bytes       size\n");
    printf("bootloader        %3d-%-3d    0x%05x     %5d\n", BL_PAGE, BL_PAGE + BL_PAGES - 1,
           0, BL_PAGES * PAGE_SIZE);
    printf("device identity   %3d        0x%05x     %5d\n", ID_PAGE, ID_PAGE * PAGE_SIZE, PAGE_SIZE);
    printf("meta bank 0       %3d-%-3d    0x%05x     %5d\n", META0_PAGE, META0_PAGE + META_PAGES - 1,
           META0_PAGE * PAGE_SIZE, META_PAGES * PAGE_SIZE);
    printf("meta bank 1       %3d-%-3d    0x%05x     %5d\n", META1_PAGE, META1_PAGE + META_PAGES - 1,
           META1_PAGE * PAGE_SIZE, META_PAGES * PAGE_SIZE);
    printf("slot 0            %3d-%-3d    0x%05x     %5d\n", SLOT0_PAGE, SLOT0_PAGE + SLOT_PAGES - 1,
           SLOT0_PAGE * PAGE_SIZE, SLOT_SIZE);
    printf("slot 1            %3d-%-3d    0x%05x     %5d\n", SLOT1_PAGE, SLOT1_PAGE + SLOT_PAGES - 1,
           SLOT1_PAGE * PAGE_SIZE, SLOT_SIZE);
    printf("meta_t %zu B, journal %d half-words, slot payload %d B\n",
           sizeof(meta_t), SECTORS_SLOT, IMG_MAX_PAYLOAD);
    flash_close(&f);
    return 0;
}

static int cmd_mkimage(const char *binpath, uint32_t ver, const char *out)
{
    size_t binlen;
    uint8_t root_key[32];
    poc_root_key(root_key);
    uint8_t *bin = read_file(binpath, &binlen);
    if (!bin) die("cannot read %s", binpath);
    uint8_t *img = malloc(SLOT_SIZE);
    if (image_build(bin, binlen, ver, root_key, img) < 0)
        die("%s is %zu B, over the %d B slot payload", binpath, binlen, IMG_MAX_PAYLOAD);
    if (write_file(out, img, SLOT_SIZE) < 0) die("cannot write %s", out);
    free(bin); free(img);
    printf("wrote %s, %d B slot image version %u\n", out, SLOT_SIZE, ver);
    return 0;
}

/* Deterministic localized change: RUNS patches of 16 bytes at pseudo-random
 * offsets. Models a release that touches a handful of functions. */
static int cmd_mutate(const char *in, const char *out, int runs, unsigned seed)
{
    size_t len;
    uint8_t *b = read_file(in, &len);
    if (!b) die("cannot read %s", in);
    srand(seed);
    for (int i = 0; i < runs; i++) {
        size_t off = ((size_t)rand() * 65536 + (size_t)rand()) % (len - 16);
        for (int k = 0; k < 16; k++) b[off + k] ^= (uint8_t)(0x5A + i);
    }
    if (write_file(out, b, len) < 0) die("cannot write %s", out);
    free(b);
    printf("wrote %s, %d patched runs of 16 B\n", out, runs);
    return 0;
}

/* Inserts NBYTES near the start, shifting everything after it. Models the
 * layout hazard in DESIGN.md section 17. */
static int cmd_shift(const char *in, const char *out, int nbytes)
{
    size_t len;
    uint8_t *b = read_file(in, &len);
    if (!b) die("cannot read %s", in);
    uint8_t *o = malloc(len);
    size_t at = 0x1000;
    memcpy(o, b, at);
    memset(o + at, 0x90, (size_t)nbytes);
    memcpy(o + at + nbytes, b + at, len - at - nbytes);
    if (write_file(out, o, len) < 0) die("cannot write %s", out);
    free(b); free(o);
    printf("wrote %s, %d B inserted at 0x%zx\n", out, nbytes, at);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "init") && argc == 3)        return cmd_init(argv[2]);
    if (!strcmp(c, "provision") && argc == 6)   return cmd_provision(argv[2], atoi(argv[3]), argv[4], (uint32_t)atoi(argv[5]));
    if (!strcmp(c, "manifest") && argc == 3)    return cmd_manifest(argv[2]);
    if (!strcmp(c, "map") && argc == 3)         return cmd_map(argv[2]);
    if (!strcmp(c, "mkimage") && argc == 5)     return cmd_mkimage(argv[2], (uint32_t)atoi(argv[3]), argv[4]);
    if (!strcmp(c, "mutate") && argc == 6)      return cmd_mutate(argv[2], argv[3], atoi(argv[4]), (unsigned)atoi(argv[5]));
    if (!strcmp(c, "shift") && argc == 5)       return cmd_shift(argv[2], argv[3], atoi(argv[4]));
    return usage();
}
