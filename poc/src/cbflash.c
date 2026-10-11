/* cb-flash: inspect the emulated flash, and build test releases. */
#include "flash.h"
#include "manifest.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  cb-flash manifest FLASH             per-sector hashes and the digest\n"
        "  cb-flash map FLASH                  the flash layout\n"
        "  cb-flash mutate IN OUT RUNS SEED    a new release: RUNS patched runs\n");
    return 2;
}

static int cmd_manifest(const char *path)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN];
    char hex[80];

    if (flash_open(&f, path) < 0) die("cannot open %s", path);
    meta_load(&f, &m);
    (void)m;
    manifest_build(&f, &man);
    for (int k = 0; k < MANIFEST_ENTRIES; k++) {
        hexdump_line(man.h[k], SECTOR_HASH_LEN, hex);
        printf("%3d %s%s", k, hex, (k % 5 == 4) ? "\n" : "   ");
    }
    if (MANIFEST_ENTRIES % 5) printf("\n");
    manifest_digest(&man, d);
    hexdump_line(d, 32, hex);
    printf("body %zu B  digest %s\n", MANIFEST_BODY_LEN, hex);
    flash_close(&f);
    return 0;
}

static int cmd_map(const char *path)
{
    flash_t f;
    if (flash_open(&f, path) < 0) die("cannot open %s", path);
    printf("region            pages      bytes       size\n");
    printf("bootloader        %3d-%-3d    0x%05x     %5d\n",
           BL_PAGE, BL_PAGE + BL_PAGES - 1, 0, BL_PAGES * PAGE_SIZE);
    printf("device identity   %3d        0x%05x     %5d\n",
           ID_PAGE, ID_PAGE * PAGE_SIZE, PAGE_SIZE);
    printf("meta bank 0       %3d-%-3d    0x%05x     %5d\n",
           META0_PAGE, META0_PAGE + META_PAGES - 1, META0_PAGE * PAGE_SIZE,
           META_PAGES * PAGE_SIZE);
    printf("meta bank 1       %3d-%-3d    0x%05x     %5d\n",
           META1_PAGE, META1_PAGE + META_PAGES - 1, META1_PAGE * PAGE_SIZE,
           META_PAGES * PAGE_SIZE);
    printf("application       %3d-%-3d    0x%05x     %5d\n",
           APP_PAGE, APP_PAGE + APP_PAGES - 1, APP_PAGE * PAGE_SIZE, SLOT_SIZE);
    flash_close(&f);
    return 0;
}

/* Deterministic localized change: RUNS patches of 16 bytes at pseudo-random
 * offsets, which is what a release that touches a few functions looks like. */
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

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "manifest") && argc == 3) return cmd_manifest(argv[2]);
    if (!strcmp(c, "map") && argc == 3)      return cmd_map(argv[2]);
    if (!strcmp(c, "mutate") && argc == 6)
        return cmd_mutate(argv[2], argv[3], atoi(argv[4]), (unsigned)atoi(argv[5]));
    return usage();
}
