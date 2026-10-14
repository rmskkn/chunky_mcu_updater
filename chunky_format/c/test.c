/* The generated C reader (chunky_format.h) against the golden vectors (golden.h): `just test-c`. */
#include <stdio.h>

#include "chunky_format.h"
#include "golden.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                \
        }                                                              \
    } while (0)

int main(void) {
    const uint8_t *plan = golden_plan;
    const uint8_t *desc = chunky_plan_header_descriptor(plan);
    const uint8_t *body = chunky_signed_descriptor_body(desc);
    const uint8_t *at;
    const uint8_t *end;
    uint32_t steps = 0;
    uint32_t last_erase = 0;
    uint32_t copy_src = 0;
    uint32_t write_dst = 0;
    uint32_t write_len = 0;

    CHECK(memcmp(chunky_plan_header_magic(plan), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) == 0);
    CHECK(chunky_plan_header_format(plan) == 2u);
    CHECK(chunky_plan_header_geometry_id(plan) == GOLDEN_GEOMETRY_ID);
    CHECK(chunky_plan_header_step_count(plan) == GOLDEN_STEP_COUNT);
    CHECK(CHUNKY_PLAN_HEADER_LEN + chunky_plan_header_body_len(plan) == sizeof golden_plan);

    CHECK(memcmp(chunky_descriptor_body_magic(body), "CKY2", 4u) == 0);
    CHECK(chunky_scheme_tag(chunky_descriptor_body_scheme(body)) == CHUNKY_SCHEME_ED25519);
    CHECK(chunky_descriptor_body_product_id(body) == GOLDEN_PRODUCT_ID);
    CHECK(chunky_descriptor_body_version(body) == GOLDEN_VERSION);
    CHECK(chunky_descriptor_body_security_counter(body) == GOLDEN_SECURITY_COUNTER);
    CHECK(chunky_descriptor_body_image_len(body) == GOLDEN_IMAGE_LEN);
    CHECK(chunky_descriptor_body_geometry_id(body) == GOLDEN_GEOMETRY_ID);
    CHECK(chunky_signed_descriptor_seal(desc) == desc + 64u);

    at = plan + CHUNKY_PLAN_HEADER_LEN;
    end = plan + sizeof golden_plan;
    while (at < end) {
        uint32_t tag = chunky_step_tag(at);
        size_t len = chunky_step_len(tag);
        CHECK(len != 0u);
        if (len == 0u || (size_t)(end - at) < len) {
            break;
        }
        switch (tag) {
        case CHUNKY_STEP_ERASE:
            last_erase = chunky_step_erase_block(at);
            break;
        case CHUNKY_STEP_COPY:
            copy_src = chunky_step_copy_src(at);
            break;
        case CHUNKY_STEP_WRITE:
            write_dst = chunky_step_write_dst(at);
            write_len = chunky_step_write_data_len(at);
            len += write_len;
            break;
        default:
            break;
        }
        at += len;
        steps++;
    }
    CHECK(at == end);
    CHECK(steps == GOLDEN_STEP_COUNT);
    CHECK(last_erase == GOLDEN_LAST_ERASE);
    CHECK(copy_src == GOLDEN_COPY_SRC);
    CHECK(write_dst == GOLDEN_WRITE_DST);
    CHECK(write_len == GOLDEN_WRITE_LEN);

    if (failures != 0) {
        printf("chunky_format C reader: %d failures\n", failures);
        return 1;
    }
    printf("chunky_format C reader: golden vectors ok\n");
    return 0;
}
