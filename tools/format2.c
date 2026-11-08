/* format2: a standalone CLI for chunky's "format 2" wire contract (SPEC.md),
 * built on lib/format2.c's reader/verifier and the vendored
 * rust/chunky_format/c accessors and golden vectors. One binary, two
 * subcommands, dispatched on argv[1] (same shape as src/cbflash.c):
 *
 *   format2 dump  [DIR]                              write the golden vectors to .bin files
 *   format2 check [-g ID] [-p ID] [-f N] PLAN [ACTIVE]  verify a plan file
 *
 * dump: writes rust/chunky_format/c/golden.h's six vendored byte arrays to
 * real files on disk, so the format-2 vectors can be poked at with
 * ordinary tools (xxd, this tool's own check, a hex editor) instead of
 * only through C arrays baked into a test binary. golden.h itself has no
 * file I/O (it is a header meant for embedding); check writes nothing
 * (see below), so dump is the only way this tool ever touches a
 * filesystem for output.
 *
 * check: verifies a format-2 plan file (SPEC.md section 5) through
 * lib/format2.c's real checks -- the seal over the sealed descriptor
 * body, per-step chunk hashes, index range and step order -- not just the
 * structural parse rust/chunky_format/c/read_plan.c does. It is the
 * verifier docs/format2.md's "If this grows into real wire usage later"
 * section describes calling format2_plan_begin()/_walk() from, minus any
 * flash: it only reports what format2.c's checks say about the bytes on
 * disk.
 *
 * check's -g/-p/-f default to the vendored golden vectors' values
 * (rust/chunky_format/c/golden.h: GOLDEN_UNSIGNED_GEOMETRY_ID et al.),
 * since `format2 dump`'s output is the only on-disk plan this tree can
 * currently produce. A plan from another source needs its own geometry
 * id and product id passed on the command line.
 *
 * ACTIVE, if given, supplies the bytes format2_plan_walk reads through
 * format2_read_active_fn to check a COPY step's source hash (SPEC.md 5.2);
 * it must be at least `chunks * chunk_len` bytes, chunk-indexed. Without
 * it, a plan with no COPY step still verifies fully; a plan with a COPY
 * step stops at the first one with "no ACTIVE available" rather than
 * skipping the check (format2.c's "refuse rather than guess" rule, see
 * its header).
 *
 * The golden vectors' real geometry (SPEC.md section 11): chunk 256 B,
 * erase 2048 B, a 4096 B slot -> chunks=16, blocks=2. golden.h's constants
 * only give chunks and image_len; chunk_len and blocks are derived here
 * the same way tests/format2_test.c derives GOLDEN_CHUNK_LEN, and are not
 * yet exposed as check's own flags -- a plan at a different geometry needs
 * those constants edited to match (the same limitation
 * format2_plan_begin() has: image_len comes from the caller's geometry,
 * not the plan). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunky_format.h"
#include "golden.h"
#include "format2.h"

static int usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  format2 dump  [DIR]                                 write the golden vectors to .bin files\n"
            "  format2 check [-g GEOMETRY_ID] [-p PRODUCT_ID] [-f FLOOR] PLAN.bin [ACTIVE.bin]\n"
            "                                                      verify a plan file (check's defaults\n"
            "                                                      match the golden vectors)\n");
    return 2;
}

/* ---- dump: golden.h's vectors -> real files ---- */

static int dump_one(const char *dir, const char *name, const uint8_t *buf, size_t len) {
    char path[512];
    FILE *f;

    if (dir != NULL && dir[0] != '\0') {
        snprintf(path, sizeof path, "%s/%s", dir, name);
    } else {
        snprintf(path, sizeof path, "%s", name);
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        perror(path);
        return 1;
    }
    if (fwrite(buf, 1u, len, f) != len) {
        fprintf(stderr, "%s: short write\n", path);
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("%-22s %6zu bytes -> %s\n", name, len, path);
    return 0;
}

/* Writes, into dir (default: current directory):
 *   signed_plan.bin, signed_manifest.bin, signed_written.bin
 *   unsigned_plan.bin, unsigned_manifest.bin, unsigned_written.bin
 *
 * signed_plan.bin / unsigned_plan.bin are real format-2 plan streams
 * (header + body, SPEC.md section 5) and are what `format2 check` expects
 * as input. *_written.bin is the raw WRITE payload data the plan's WRITE
 * steps carry (golden.h's data_at offsets index into it); *_manifest.bin
 * is a format-2 manifest (SPEC.md section 7). */
static int cmd_dump(int argc, char **argv) {
    const char *dir = argc > 0 ? argv[0] : ".";
    int rc = 0;

    rc |= dump_one(dir, "signed_plan.bin", golden_signed_plan, sizeof golden_signed_plan);
    rc |= dump_one(dir, "signed_manifest.bin", golden_signed_manifest, sizeof golden_signed_manifest);
    rc |= dump_one(dir, "signed_written.bin", golden_signed_written, sizeof golden_signed_written);
    rc |= dump_one(dir, "unsigned_plan.bin", golden_unsigned_plan, sizeof golden_unsigned_plan);
    rc |= dump_one(dir, "unsigned_manifest.bin", golden_unsigned_manifest, sizeof golden_unsigned_manifest);
    rc |= dump_one(dir, "unsigned_written.bin", golden_unsigned_written, sizeof golden_unsigned_written);

    return rc == 0 ? 0 : 1;
}

/* ---- check: verify a plan file through lib/format2.c ---- */

#define MAX_PLAN (64u * 1024u)
#define MAX_ACTIVE (64u * 1024u)

static uint8_t plan_buf[MAX_PLAN];
static uint8_t active_buf[MAX_ACTIVE];
static size_t active_len;
static int have_active;

static size_t read_file(const char *path, uint8_t *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        perror(path);
        return (size_t)-1;
    }
    n = fread(buf, 1u, cap, f);
    if (!feof(f)) {
        fprintf(stderr, "%s: larger than %zu bytes\n", path, cap);
        fclose(f);
        return (size_t)-1;
    }
    fclose(f);
    return n;
}

static int read_active(void *ctx, uint16_t chunk_index, uint8_t *buf, uint16_t chunk_len) {
    size_t off = (size_t)chunk_index * (size_t)chunk_len;
    (void)ctx;
    if (!have_active || off + chunk_len > active_len) {
        return -1;
    }
    memcpy(buf, active_buf + off, chunk_len);
    return 0;
}

static const char *step_name(uint8_t op) {
    switch (op) {
    case CHUNKY_STEP_ERASE: return "ERASE";
    case CHUNKY_STEP_COPY:  return "COPY";
    case CHUNKY_STEP_WRITE: return "WRITE";
    default:                return "?";
    }
}

static int on_step(void *ctx, const format2_step_t *s, uint32_t index) {
    (void)ctx;
    switch (s->op) {
    case CHUNKY_STEP_ERASE:
        printf("  %4u  ERASE  block %u\n", index, s->a);
        break;
    case CHUNKY_STEP_COPY:
        printf("  %4u  COPY   dst %u <- active chunk %u  (hash checked)\n", index, s->a, s->b);
        break;
    case CHUNKY_STEP_WRITE:
        printf("  %4u  WRITE  dst %u, %u bytes  (hash checked)\n", index, s->a, s->b);
        break;
    default:
        printf("  %4u  %s?\n", index, step_name(s->op));
        break;
    }
    return 0;
}

static int cmd_check(int argc, char **argv) {
    uint32_t geometry_id = GOLDEN_UNSIGNED_GEOMETRY_ID;
    uint32_t product_id = GOLDEN_UNSIGNED_PRODUCT_ID;
    uint32_t floor = 0;
    const char *plan_path = NULL;
    const char *active_path = NULL;
    size_t plan_len;
    int i;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            geometry_id = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            product_id = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            floor = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (plan_path == NULL) {
            plan_path = argv[i];
        } else if (active_path == NULL) {
            active_path = argv[i];
        } else {
            return usage();
        }
    }
    if (plan_path == NULL) {
        return usage();
    }

    plan_len = read_file(plan_path, plan_buf, sizeof plan_buf);
    if (plan_len == (size_t)-1) {
        return 1;
    }

    if (active_path != NULL) {
        active_len = read_file(active_path, active_buf, sizeof active_buf);
        if (active_len == (size_t)-1) {
            return 1;
        }
        have_active = 1;
    }

    /* The golden fixture's geometry (SPEC.md section 11): chunk 256 B,
     * erase block 2048 B, slot 4096 B -> chunks = 16, blocks = 2,
     * image_len = 0xf80 (3968). See the file header for why these three
     * are not yet their own flags. */
    {
        const uint32_t chunk_len = 256;
        const uint32_t chunks = GOLDEN_UNSIGNED_CHUNKS;
        const uint32_t blocks = 2;
        const uint32_t image_len = GOLDEN_UNSIGNED_IMAGE_LEN;
        const uint8_t *body = NULL;
        format2_err_t e;
        int rc;

        printf("%s: %zu bytes, geometry 0x%08x, product 0x%08x, floor %u\n",
               plan_path, plan_len, geometry_id, product_id, floor);

        e = format2_plan_begin(plan_buf, plan_len, geometry_id, product_id, floor, image_len, &body);
        if (e != FORMAT2_OK) {
            printf("plan_begin: REFUSED (%s)\n", format2_err_str(e));
            return 1;
        }
        printf("plan_begin: OK  version %u, security counter %u, image %u bytes\n",
               chunky_descriptor_body_version(body),
               chunky_descriptor_body_security_counter(body),
               chunky_descriptor_body_image_len(body));

        {
            uint32_t step_count = chunky_plan_header_step_count(plan_buf);
            uint8_t scratch[256];

            if (!have_active) {
                printf("(no ACTIVE.bin given: a COPY step will stop the walk at FORMAT2_ENOACTIVE)\n");
            }
            rc = format2_plan_walk(plan_buf, plan_len, step_count, blocks, chunks, (uint16_t)chunk_len,
                                   have_active ? read_active : NULL, NULL, scratch, on_step, NULL);
            if (rc != 0) {
                if (rc < 0) {
                    printf("plan_walk: REFUSED (%s)\n", format2_err_str((format2_err_t)(-rc)));
                } else {
                    printf("plan_walk: step callback stopped the walk (%d)\n", rc);
                }
                return 1;
            }
            printf("plan_walk: OK  %u steps, every hash checked\n", step_count);
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return usage();
    }
    if (strcmp(argv[1], "dump") == 0) {
        return cmd_dump(argc - 2, argv + 2);
    }
    if (strcmp(argv[1], "check") == 0) {
        return cmd_check(argc - 2, argv + 2);
    }
    return usage();
}
