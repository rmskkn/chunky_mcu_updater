/* cb-flash: inspect the emulated flash, and build test releases. */
#include "flash.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_manifest.h"
#include "sha256.h"
#include "util.h"
#include "flash_file.h"
#include "cli.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* format2_manifest_build()'s own stated bound (lib/format2_manifest.h). */
#define MANIFEST_BUF_CAP (13u + 2u * (size_t)FORMAT2_CHUNKS * 9u)

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

/* Flat entry index 0..2*chunks-1: active_slot's chunks then dfu_slot's, format2_manifest_build()'s ordering. */
typedef struct { int printed; } print_ctx_t;

static int print_entry(void *ctx, const format2_entry_t *e, uint32_t index)
{
    print_ctx_t *c = ctx;
    char hex[32];

    if (e->tag == CHUNKY_CHUNK_ENTRY_HASH) {
        hexdump_line(e->hash, FORMAT2_CHUNK_HASH_LEN, hex);
        printf("%3u %-16s", index, hex);
    } else {
        printf("%3u %-16s", index, e->tag == CHUNKY_CHUNK_ENTRY_BLANK ? "BLANK" : "UNREADABLE");
    }
    printf("%s", (++c->printed % 5 == 0) ? "\n" : "  ");
    return 0;
}

static int cmd_manifest(const char *path)
{
    flash_t f;
    meta_t m;
    uint8_t buf[MANIFEST_BUF_CAP];
    size_t mlen;
    uint16_t chunks = 0;
    format2_err_t fe;
    uint8_t d[32];
    char hex[80];
    print_ctx_t pc = { 0 };

    if (flash_file_open(&f, path) < 0) return cli_fail("cannot open %s", path);
    meta_load(&f, &m);
    mlen = format2_manifest_build(&f, m.active_slot, OTHER_SLOT(m.active_slot), buf, sizeof buf);
    if (mlen == 0) { flash_file_close(&f); return cli_fail("manifest build failed (buffer too small)"); }
    fe = format2_manifest_begin(buf, mlen, format2_geometry_id(), &chunks);
    if (fe != FORMAT2_OK) { flash_file_close(&f); return cli_fail("manifest: %s", format2_err_str(fe)); }

    format2_manifest_walk(buf, mlen, (uint32_t)2u * chunks, print_entry, &pc);
    if (pc.printed % 5) printf("\n");
    sha256(buf, mlen, d);
    hexdump_line(d, 32, hex);
    printf("body %zu B  digest %s\n", mlen, hex);
    flash_file_close(&f);
    return 0;
}

/* Reverse lookup. Index to hash is a subscript; hash to index is a scan, and
 * can match more than one chunk (an erased page hashes the same everywhere); any match is an equally good COPY source, which is why find_active_match() is a content lookup, not positional. */
typedef struct {
    const uint8_t *want;
    size_t         wlen;
    uint16_t       chunks;
    int            active_slot, dfu_slot;
    int            hits;
} find_ctx_t;

static int find_entry(void *ctx, const format2_entry_t *e, uint32_t index)
{
    find_ctx_t *c = ctx;
    char hex[32];

    if (e->tag != CHUNKY_CHUNK_ENTRY_HASH || memcmp(e->hash, c->want, c->wlen)) return 0;
    int slot = index < c->chunks ? c->active_slot : c->dfu_slot;
    uint32_t i = index < c->chunks ? index : index - c->chunks;
    hexdump_line(e->hash, FORMAT2_CHUNK_HASH_LEN, hex);
    printf("slot %c chunk %3u  %s  at 0x%05zx\n", 'A' + slot, i, hex,
           slot_base(slot) + (size_t)i * FORMAT2_CHUNK_LEN);
    c->hits++;
    return 0;
}

static int cmd_find(const char *path, const char *hex)
{
    flash_t f;
    meta_t m;
    uint8_t buf[MANIFEST_BUF_CAP];
    size_t mlen;
    uint16_t chunks = 0;
    format2_err_t fe;
    uint8_t want[FORMAT2_CHUNK_HASH_LEN];
    size_t n = strlen(hex) / 2;

    if (n == 0 || n > FORMAT2_CHUNK_HASH_LEN || strlen(hex) % 2)
        return cli_fail("hash must be 2 to %u hex bytes", (unsigned)FORMAT2_CHUNK_HASH_LEN);
    for (size_t i = 0; i < n; i++) {
        unsigned b;
        if (sscanf(hex + i * 2, "%2x", &b) != 1) return cli_fail("bad hex in %s", hex);
        want[i] = (uint8_t)b;
    }

    if (flash_file_open(&f, path) < 0) return cli_fail("cannot open %s", path);
    meta_load(&f, &m);
    mlen = format2_manifest_build(&f, m.active_slot, OTHER_SLOT(m.active_slot), buf, sizeof buf);
    fe = mlen ? format2_manifest_begin(buf, mlen, format2_geometry_id(), &chunks) : FORMAT2_ESHORT;
    if (fe != FORMAT2_OK) { flash_file_close(&f); return cli_fail("manifest: %s", format2_err_str(fe)); }

    find_ctx_t fc = { want, n, chunks, m.active_slot, OTHER_SLOT(m.active_slot), 0 };
    format2_manifest_walk(buf, mlen, (uint32_t)2u * chunks, find_entry, &fc);
    printf("%d of %u chunks match %s%s\n", fc.hits, 2u * (unsigned)chunks, hex,
           n < FORMAT2_CHUNK_HASH_LEN ? " (prefix)" : "");
    flash_file_close(&f);
    return fc.hits ? 0 : 1;
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
    printf("ERASE_SIZE=%d SECTOR_SIZE=%d SECTORS=%d SLOT_SIZE=%d PAYLOAD_MAX=%d "
           "geometry_id=0x%08x\n",
           ERASE_SIZE, SECTOR_SIZE, SECTORS_SLOT, SLOT_SIZE, (int)FORMAT2_IMAGE_LEN,
           format2_geometry_id());
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
