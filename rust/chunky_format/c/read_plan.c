/* How a C peer reads a plan with chunky_format.h: the header checks that need no key (SPEC.md §6), the
 * descriptor's fields, then every step, each through a view that checks its length before a field can be read.
 * `make example` runs it on the golden plans; `build/read_plan PLAN.bin` reads any plan.
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
    chunky_plan_header_view_t h;
    chunky_signed_descriptor_view_t desc;
    chunky_descriptor_body_view_t body;
    size_t at;
    uint32_t steps;
    uint32_t n;

    if (chunky_plan_header_view(p, len, &h) != 0) {
        printf("too short for a plan header: %zu bytes\n", len);
        return -1;
    }
    if (memcmp(chunky_plan_header_magic(h), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) != 0 ||
        chunky_plan_header_format(h) != 2u) {
        printf("not a format 2 plan\n");
        return -1;
    }
    if ((size_t)chunky_plan_header_body_len(h) != len - h.len) {
        printf("body length %lu, but %zu bytes follow the header\n", (unsigned long)chunky_plan_header_body_len(h),
               len - h.len);
        return -1;
    }
    desc = chunky_plan_header_descriptor(h);
    if (chunky_descriptor_body_view(chunky_signed_descriptor_body(desc), CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT, &body) != 0) {
        printf("descriptor: unknown scheme\n");
        return -1;
    }
    steps = chunky_plan_header_step_count(h);
    printf("geometry 0x%08lx, %lu steps; image: product 0x%08lx, version %lu, counter %lu, %lu bytes, %s\n",
           (unsigned long)chunky_plan_header_geometry_id(h), (unsigned long)steps,
           (unsigned long)chunky_descriptor_body_product_id(body), (unsigned long)chunky_descriptor_body_version(body),
           (unsigned long)chunky_descriptor_body_security_counter(body),
           (unsigned long)chunky_descriptor_body_image_len(body),
           scheme_name(chunky_scheme_tag(chunky_descriptor_body_scheme(body))));

    at = h.len;
    for (n = 0; n < steps; n++) {
        chunky_step_view_t s;
        const chunky_step_erase_view_t *erase;
        const chunky_step_copy_view_t *copy;
        const chunky_step_write_view_t *write;
        if (at == len) {
            printf("step %lu: the body ends early\n", (unsigned long)n);
            return -1;
        }
        if (chunky_step_view(p + at, len - at, &s) != 0) {
            printf("step %lu: unknown op or truncated\n", (unsigned long)n);
            return -1;
        }
        if (chunky_step_as_erase(&s, &erase) == 0) {
            printf("  %3lu ERASE block %u\n", (unsigned long)n, chunky_step_erase_block(erase));
        } else if (chunky_step_as_copy(&s, &copy) == 0) {
            printf("  %3lu COPY  chunk %u <- active chunk %u\n", (unsigned long)n, chunky_step_copy_dst(copy),
                   chunky_step_copy_src(copy));
        } else if (chunky_step_as_write(&s, &write) == 0) {
            printf("  %3lu WRITE chunk %u, %u bytes\n", (unsigned long)n, chunky_step_write_dst(write),
                   chunky_step_write_data_count(write));
        } else {
            printf("step %lu: unknown op\n", (unsigned long)n);
            return -1;
        }
        at += s.len;
    }
    if (at != len) {
        printf("%zu bytes after the last step\n", len - at);
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
