/* How a C peer reads a plan with chunky_format.h: the header checks that need no key (SPEC.md §6), the
 * descriptor's fields, then every step, each length checked before it is read. `make example` runs it on the
 * golden plans; `build/read_plan PLAN.bin` reads any plan.
 *
 * Not shown: checking the descriptor's seal (Ed25519, or SHA-256 for the unsigned scheme) over the 64 body
 * bytes. A device does that after these checks and before it acts on a single step. */
#include <stdio.h>
#include <string.h>

#include "chunky_format.h"

#define MAX_PLAN (64u * 1024u)

static uint8_t plan[MAX_PLAN];

static const char *scheme_name(uint32_t tag) {
    switch (tag) {
    case CHUNKY_SCHEME_ED25519:
        return "ed25519";
    case CHUNKY_SCHEME_UNSIGNED:
        return "unsigned";
    default:
        return "unknown";
    }
}

/* 0 when the plan is well formed; prints what it holds */
static int read_plan(const uint8_t *p, size_t len) {
    const uint8_t *desc;
    const uint8_t *body;
    const uint8_t *at;
    const uint8_t *end = p + len;
    uint32_t steps;
    uint32_t n;

    if (len < CHUNKY_PLAN_HEADER_LEN) {
        printf("too short for a plan header: %zu bytes\n", len);
        return -1;
    }
    if (memcmp(chunky_plan_header_magic(p), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) != 0 ||
        chunky_plan_header_format(p) != 2u) {
        printf("not a format 2 plan\n");
        return -1;
    }
    if ((size_t)chunky_plan_header_body_len(p) != len - CHUNKY_PLAN_HEADER_LEN) {
        printf("body length %lu, but %zu bytes follow the header\n", (unsigned long)chunky_plan_header_body_len(p),
               len - CHUNKY_PLAN_HEADER_LEN);
        return -1;
    }
    desc = chunky_plan_header_descriptor(p);
    body = chunky_signed_descriptor_body(desc);
    steps = chunky_plan_header_step_count(p);
    printf("geometry 0x%08lx, %lu steps; image: product 0x%08lx, version %lu, counter %lu, %lu bytes, %s\n",
           (unsigned long)chunky_plan_header_geometry_id(p), (unsigned long)steps,
           (unsigned long)chunky_descriptor_body_product_id(body), (unsigned long)chunky_descriptor_body_version(body),
           (unsigned long)chunky_descriptor_body_security_counter(body),
           (unsigned long)chunky_descriptor_body_image_len(body),
           scheme_name(chunky_scheme_tag(chunky_descriptor_body_scheme(body))));

    at = p + CHUNKY_PLAN_HEADER_LEN;
    for (n = 0; n < steps; n++) {
        const uint8_t *t;
        chunky_step_tail_t tl;
        if (at >= end) {
            printf("step %lu: the body ends early\n", (unsigned long)n);
            return -1;
        }
        t = at + CHUNKY_STEP_TAG_LEN;
        if (chunky_step_tail(at, &t, end, &tl) != 0) {
            printf("step %lu: unknown op or truncated\n", (unsigned long)n);
            return -1;
        }
        switch (chunky_step_tag(at)) {
        case CHUNKY_STEP_ERASE:
            printf("  %3lu ERASE block %u\n", (unsigned long)n, chunky_step_erase_block(tl.head));
            break;
        case CHUNKY_STEP_COPY:
            printf("  %3lu COPY  chunk %u <- active chunk %u\n", (unsigned long)n, chunky_step_copy_dst(tl.head),
                   chunky_step_copy_src(tl.head));
            break;
        case CHUNKY_STEP_WRITE:
            printf("  %3lu WRITE chunk %u, %u bytes\n", (unsigned long)n, chunky_step_write_dst(tl.head),
                   chunky_step_write_data_count(tl.head));
            break;
        default:
            printf("step %lu: unknown op\n", (unsigned long)n);
            return -1;
        }
        at = t;
    }
    if (at != end) {
        printf("%zu bytes after the last step\n", (size_t)(end - at));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    FILE *f;
    size_t len;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PLAN.bin\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL) {
        perror(argv[1]);
        return 1;
    }
    len = fread(plan, 1u, sizeof plan, f);
    if (!feof(f)) {
        fprintf(stderr, "%s: larger than %u bytes\n", argv[1], MAX_PLAN);
        fclose(f);
        return 1;
    }
    fclose(f);
    printf("%s: ", argv[1]);
    return read_plan(plan, len) == 0 ? 0 : 1;
}
