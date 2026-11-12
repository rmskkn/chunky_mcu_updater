/* format2: a standalone CLI for chunky's "format 2" wire contract (SPEC.md),
 * built on lib/format2.c's reader/verifier and the vendored
 * rust/chunky_format/c views and golden vectors. One binary, three
 * subcommands, dispatched on argv[1] (same shape as src/cbflash.c):
 *
 *   format2 dump  [DIR]                              write the golden vectors to .bin files
 *   format2 check [-g ID] [-p ID] [-f N] PLAN [ACTIVE]  verify a plan file
 *   format2 gen   [-c LEN] [-e LEN] ACTIVE NEW PLAN     plan NEW from ACTIVE
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
 * not the plan).
 *
 * gen: the planner SPEC.md section 7.3 describes, offline -- it takes
 * ACTIVE (the slot as it runs today) and NEW (the target image: payload,
 * 0xFF padding, and NEW's own 128-byte descriptor already in its last
 * bytes, the same shape as the vendored old.bin/new.bin vectors), and
 * writes a plan that turns ACTIVE into NEW. NEW's descriptor is copied
 * into the plan header verbatim (SPEC.md 5.1: "byte for byte the one that
 * ends the image"); gen does not build or sign one -- that happens at
 * release time (SPEC.md 12.3), out of scope here. A sanity check (magic,
 * format, image length, image SHA-256 against NEW's own descriptor)
 * catches a wrong file or a wrong -c/-e before any plan is written.
 *
 * It has no DFU manifest to say which chunks the device already has
 * right or already blank there (SPEC.md section 7), so it takes the safe
 * path: every erase block NEW touches is erased (an already-blank block
 * gets an erase it did not strictly need, which SPEC.md 5.4 allows but
 * does not require), and every chunk that is not blank after that erase
 * is programmed -- by COPY when ACTIVE holds the same bytes anywhere
 * (SPEC.md 7.3's dedup rule), else by WRITE. -c/-e set chunk and erase
 * block size (default 256/2048, the vendored vectors' geometry); the
 * geometry id written to the header comes from NEW's own descriptor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chunky_format.h"
#include "golden.h"
#include "format2.h"
#include "sha256.h"

static int usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  format2 dump  [DIR]                                 write the golden vectors to .bin files\n"
            "  format2 check [-g GEOMETRY_ID] [-p PRODUCT_ID] [-f FLOOR] PLAN.bin [ACTIVE.bin]\n"
            "                                                      verify a plan file (check's defaults\n"
            "                                                      match the golden vectors)\n"
            "  format2 gen   [-c CHUNK_LEN] [-e ERASE_LEN] ACTIVE.bin NEW.bin PLAN.bin\n"
            "                                                      plan NEW from ACTIVE (defaults: chunk 256,\n"
            "                                                      erase 2048, the vendored vectors' geometry)\n");
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
        chunky_descriptor_body_view_t body;
        chunky_plan_header_view_t h;
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

        if (chunky_plan_header_view(plan_buf, plan_len, &h) != 0) {
            printf("plan header: too short\n"); /* plan_begin already refused that */
            return 1;
        }
        {
            uint32_t step_count = chunky_plan_header_step_count(h);
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

/* ---- gen: plan a format-2 upgrade from ACTIVE and the target image ---- */

/* Geometry defaults match the vendored vectors (SPEC.md section 11): chunk
 * 256 B, erase block 2048 B. Not derived from a device catalog entry
 * (format 2 has none in this tree); -c/-e override for a different slot. */
#define GEN_DEFAULT_CHUNK_LEN 256u
#define GEN_DEFAULT_ERASE_LEN 2048u

static uint8_t *read_whole_file(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long sz;
    uint8_t *buf;

    if (f == NULL) {
        perror(path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        perror(path);
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)sz > 0u ? (size_t)sz : 1u);
    if (buf == NULL) {
        fprintf(stderr, "%s: out of memory\n", path);
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len_out = (size_t)sz;
    return buf;
}

static void put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
}

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8u) & 0xFFu);
    p[2] = (uint8_t)((v >> 16u) & 0xFFu);
    p[3] = (uint8_t)((v >> 24u) & 0xFFu);
}

static int is_blank(const uint8_t *p, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

/* First ACTIVE chunk whose chunk_len bytes equal target's, -1 if none
 * (SPEC.md 7.3: "a chunk is programmed by COPY when ACTIVE holds the same
 * bytes anywhere, else by WRITE"). */
static long find_active_match(const uint8_t *active, uint32_t chunks, uint32_t chunk_len, const uint8_t *target) {
    uint32_t i;
    for (i = 0; i < chunks; i++) {
        if (memcmp(active + (size_t)i * chunk_len, target, chunk_len) == 0) {
            return (long)i;
        }
    }
    return -1;
}

static int cmd_gen(int argc, char **argv) {
    uint32_t chunk_len = GEN_DEFAULT_CHUNK_LEN;
    uint32_t erase_len = GEN_DEFAULT_ERASE_LEN;
    const char *active_path = NULL;
    const char *new_path = NULL;
    const char *out_path = NULL;
    uint8_t *active = NULL;
    uint8_t *image = NULL;
    size_t active_len = 0;
    size_t image_len_total = 0;
    int i;
    int rc = 1;

    for (i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            chunk_len = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            erase_len = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (active_path == NULL) {
            active_path = argv[i];
        } else if (new_path == NULL) {
            new_path = argv[i];
        } else if (out_path == NULL) {
            out_path = argv[i];
        } else {
            return usage();
        }
    }
    if (out_path == NULL) {
        return usage();
    }

    active = read_whole_file(active_path, &active_len);
    if (active == NULL) {
        return 1;
    }
    image = read_whole_file(new_path, &image_len_total);
    if (image == NULL) {
        free(active);
        return 1;
    }

    if (active_len != image_len_total) {
        fprintf(stderr, "%s is %zu bytes, %s is %zu bytes: ACTIVE and the target must be the same slot size\n",
                active_path, active_len, new_path, image_len_total);
        goto done;
    }
    if (chunk_len == 0u || chunk_len > 0xFFFFu || erase_len == 0u || erase_len % chunk_len != 0u) {
        fprintf(stderr, "bad geometry: chunk %u, erase %u (erase must be a nonzero multiple of chunk, chunk must fit a u16)\n",
                chunk_len, erase_len);
        goto done;
    }
    if (image_len_total <= (size_t)CHUNKY_SIGNED_DESCRIPTOR_LEN ||
        image_len_total % chunk_len != 0u || image_len_total % erase_len != 0u) {
        fprintf(stderr, "%s: %zu bytes does not divide evenly into chunk %u / erase %u, or is too small to hold a descriptor\n",
                new_path, image_len_total, chunk_len, erase_len);
        goto done;
    }

    {
        uint32_t chunks = (uint32_t)(image_len_total / chunk_len);
        uint32_t blocks = (uint32_t)(image_len_total / erase_len);
        uint32_t chunks_per_block = erase_len / chunk_len;
        chunky_signed_descriptor_view_t descriptor;
        chunky_descriptor_body_view_t body;
        uint32_t image_len = (uint32_t)(image_len_total - (size_t)CHUNKY_SIGNED_DESCRIPTOR_LEN);
        uint32_t geometry_id;
        uint8_t sha[SHA256_LEN];
        size_t out_cap;
        uint8_t *out;
        uint8_t *p;
        uint32_t step_count = 0;
        uint32_t body_len;
        uint32_t b, c;
        FILE *f;

        if (chunks == 0u || chunks > 0xFFFFu || blocks == 0u || blocks > 0xFFFFu) {
            fprintf(stderr, "%s: %u chunks / %u blocks does not fit this format's u16 indices\n", new_path, chunks, blocks);
            goto done;
        }
        /* the last 128 bytes are there (checked above); the body's view
         * refuses an unknown scheme */
        if (chunky_signed_descriptor_view(image + (image_len_total - (size_t)CHUNKY_SIGNED_DESCRIPTOR_LEN),
                                          (size_t)CHUNKY_SIGNED_DESCRIPTOR_LEN, &descriptor) != 0 ||
            chunky_descriptor_body_view(chunky_signed_descriptor_body(descriptor), CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT,
                                        &body) != 0 ||
            memcmp(chunky_descriptor_body_magic(body), "CKY2", CHUNKY_DESCRIPTOR_BODY_MAGIC_COUNT) != 0 ||
            chunky_descriptor_body_format(body) != 2u) {
            fprintf(stderr, "%s: last %u bytes are not a format-2 descriptor (bad magic, format or scheme)\n",
                    new_path, CHUNKY_SIGNED_DESCRIPTOR_LEN);
            goto done;
        }
        if (chunky_descriptor_body_image_len(body) != image_len) {
            fprintf(stderr, "%s: descriptor image length %u does not match the slot's image length %u (-c/-e geometry wrong?)\n",
                    new_path, chunky_descriptor_body_image_len(body), image_len);
            goto done;
        }
        sha256(image, image_len, sha);
        if (memcmp(sha, chunky_descriptor_body_image_sha256(body), (size_t)CHUNKY_DESCRIPTOR_BODY_IMAGE_SHA256_COUNT) != 0) {
            fprintf(stderr, "%s: image SHA-256 does not match its own descriptor\n", new_path);
            goto done;
        }
        geometry_id = chunky_descriptor_body_geometry_id(body);

        out_cap = (size_t)CHUNKY_PLAN_HEADER_LEN + (size_t)blocks * 3u + (size_t)chunks * (13u + chunk_len);
        out = malloc(out_cap);
        if (out == NULL) {
            fprintf(stderr, "out of memory (%zu bytes)\n", out_cap);
            goto done;
        }
        p = out + CHUNKY_PLAN_HEADER_LEN;

        /* SPEC.md 5.4/7.3: ascending block, then ascending chunk, erasing
         * every block it touches (no DFU manifest to say it is already
         * right or already blank there -- see the file header) and
         * skipping only a target chunk that erase alone already matches. */
        for (b = 0; b < blocks; b++) {
            *p++ = CHUNKY_STEP_ERASE;
            put_u16(p, (uint16_t)b);
            p += 2;
            step_count++;

            for (c = b * chunks_per_block; c < (b + 1u) * chunks_per_block; c++) {
                const uint8_t *target = image + (size_t)c * chunk_len;
                long src;
                uint8_t hash[FORMAT2_CHUNK_HASH_LEN];

                if (is_blank(target, chunk_len)) {
                    continue;
                }
                format2_chunk_hash(target, chunk_len, hash);
                src = find_active_match(active, chunks, chunk_len, target);
                if (src >= 0) {
                    *p++ = CHUNKY_STEP_COPY;
                    put_u16(p, (uint16_t)c);
                    p += 2;
                    put_u16(p, (uint16_t)src);
                    p += 2;
                    memcpy(p, hash, FORMAT2_CHUNK_HASH_LEN);
                    p += FORMAT2_CHUNK_HASH_LEN;
                } else {
                    *p++ = CHUNKY_STEP_WRITE;
                    put_u16(p, (uint16_t)c);
                    p += 2;
                    put_u16(p, (uint16_t)chunk_len);
                    p += 2;
                    memcpy(p, hash, FORMAT2_CHUNK_HASH_LEN);
                    p += FORMAT2_CHUNK_HASH_LEN;
                    memcpy(p, target, chunk_len);
                    p += chunk_len;
                }
                step_count++;
            }
        }

        body_len = (uint32_t)(p - (out + CHUNKY_PLAN_HEADER_LEN));

        memcpy(out, "CKP2", 4u);
        out[4] = 2u;
        put_u32(out + 5, geometry_id);
        put_u32(out + 9, step_count);
        put_u32(out + 13, body_len);
        memcpy(out + 17, descriptor.p, CHUNKY_SIGNED_DESCRIPTOR_LEN);

        f = fopen(out_path, "wb");
        if (f == NULL) {
            perror(out_path);
            free(out);
            goto done;
        }
        if (fwrite(out, 1u, (size_t)CHUNKY_PLAN_HEADER_LEN + body_len, f) != (size_t)CHUNKY_PLAN_HEADER_LEN + body_len) {
            fprintf(stderr, "%s: short write\n", out_path);
            fclose(f);
            free(out);
            goto done;
        }
        fclose(f);
        printf("%s: %u steps, %u body bytes -> %s (geometry 0x%08x)\n",
               new_path, step_count, body_len, out_path, geometry_id);
        free(out);
        rc = 0;
    }

done:
    free(active);
    free(image);
    return rc;
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
    if (strcmp(argv[1], "gen") == 0) {
        return cmd_gen(argc - 2, argv + 2);
    }
    return usage();
}
