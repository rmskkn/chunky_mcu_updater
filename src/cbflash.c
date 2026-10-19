/* cb-flash: inspect the emulated flash, and build test releases. */
#include "flash.h"
#include "manifest.h"
#include "image.h"
#include "util.h"
#include "flash_file.h"
#include "cli.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fprintf(stderr,
        "usage:\n"
        "  cb-flash manifest FLASH             per-sector hashes and the digest\n"
        "  cb-flash map                        this build's flash layout\n"
        "  cb-flash geometry                   this build's geometry, shell-sourceable\n"
        "  cb-flash mutate IN OUT RUNS SEED    a new release: RUNS patched runs\n"
        "  cb-flash find FLASH HASH            which sectors hold this hash\n");
    return 2;
}

static int cmd_manifest(const char *path)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN];
    char hex[80];

    if (flash_file_open(&f, path) < 0) return cli_fail("cannot open %s", path);
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
    flash_file_close(&f);
    return 0;
}

/* Reverse lookup. Index to hash is a subscript; hash to index is a scan, and
 * it can match more than one sector -- an erased page hashes the same wherever
 * it is. Any match is an equally good copy source, which is what makes the
 * planner's rule 2 a content lookup rather than a positional one. */
static int cmd_find(const char *path, const char *hex)
{
    flash_t f;
    manifest_t man;
    uint8_t want[SECTOR_HASH_LEN];
    size_t n = strlen(hex) / 2;
    char buf[64];

    if (n == 0 || n > SECTOR_HASH_LEN || strlen(hex) % 2)
        return cli_fail("hash must be 2 to %d hex bytes", SECTOR_HASH_LEN);
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return cli_fail("bad hex in %s", hex);
        want[i] = (uint8_t)b;
    }

    if (flash_file_open(&f, path) < 0) return cli_fail("cannot open %s", path);
    manifest_build(&f, &man);

    int hits = 0;
    for (int i = 0; i < SECTORS_SLOT; i++)
        if (!memcmp(man.h[i], want, n)) {
            hexdump_line(man.h[i], SECTOR_HASH_LEN, buf);
            printf("sector %3d  %s  at 0x%05zx\n", i, buf,
                   APP_BASE + (size_t)i * SECTOR_SIZE);
            hits++;
        }
    printf("%d of %d sectors match %s%s\n", hits, SECTORS_SLOT, hex,
           n < SECTOR_HASH_LEN ? " (prefix)" : "");
    flash_file_close(&f);
    return hits ? 0 : 1;
}

/* The map straight from the generated geometry.h: no flash file, since the
 * layout is a property of the build, not of one device's contents. */
static int cmd_map(void)
{
    printf("%s, erase unit %d B, sector %d B, sector hash %d B\n",
           CB_DEVICE_NAME, ERASE_SIZE, SECTOR_SIZE, SECTOR_HASH_LEN);
    printf("region       units      offset    address       size  protect\n");
#define ROW(name, base, size, protect)                                        \
    printf("%-12s %3u-%-6u 0x%05x   0x%08x  %6u  %s\n", name,               \
           (unsigned)((base) / ERASE_SIZE),                                    \
           (unsigned)(((base) + (size)) / ERASE_SIZE - 1), (unsigned)(base),   \
           (unsigned)(CB_FLASH_BASE + (base)), (unsigned)(size),               \
           (protect) ? "yes" : "no");
    CB_REGION_LIST(ROW)
#undef ROW
    return 0;
}

/* One line the test scripts eval, so their sizes follow the build. */
static int cmd_geometry(void)
{
    printf("ERASE_SIZE=%d SECTOR_SIZE=%d SECTORS=%d SLOT_SIZE=%d PAYLOAD_MAX=%d DESC_LEN=%d "
           "SECTOR_HASH_LEN=%d\n",
           ERASE_SIZE, SECTOR_SIZE, SECTORS_SLOT, SLOT_SIZE, IMG_MAX_PAYLOAD, DESC_LEN,
           SECTOR_HASH_LEN);
    return 0;
}

/* Deterministic localized change: RUNS patches of 16 bytes at pseudo-random
 * offsets, which is what a release that touches a few functions looks like. */
static int cmd_mutate(const char *in, const char *out, int runs, unsigned seed)
{
    size_t len;
    uint8_t *b = read_file(in, &len);
    if (!b) return cli_fail("cannot read %s", in);
    srand(seed);
    for (int i = 0; i < runs; i++) {
        size_t off = ((size_t)rand() * 65536 + (size_t)rand()) % (len - 16);
        for (int k = 0; k < 16; k++) b[off + k] ^= (uint8_t)(0x5A + i);
    }
    if (write_file(out, b, len) < 0) { free(b); return cli_fail("cannot write %s", out); }
    free(b);
    printf("wrote %s, %d patched runs of 16 B\n", out, runs);
    return 0;
}

static int run(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *c = argv[1];
    if (!strcmp(c, "manifest") && argc == 3) return cmd_manifest(argv[2]);
    if (!strcmp(c, "map") && argc == 2)      return cmd_map();
    if (!strcmp(c, "geometry") && argc == 2) return cmd_geometry();
    if (!strcmp(c, "find") && argc == 4)     return cmd_find(argv[2], argv[3]);
    if (!strcmp(c, "mutate") && argc == 6)
        return cmd_mutate(argv[2], argv[3], atoi(argv[4]), (unsigned)atoi(argv[5]));
    return usage();
}

/* A command's -1 (already reported by cli_fail) is exit status 1. */
int main(int argc, char **argv)
{
    int rc = run(argc, argv);
    return rc < 0 ? 1 : rc;
}
