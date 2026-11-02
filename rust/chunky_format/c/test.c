/* The generated C reader (chunky_format.h) against the golden vectors (golden.h), signed and unsigned: it walks
 * the whole plan and the whole manifest and compares every field with what the planner and the manifest say
 * (the tables of golden.h). `make vectors`. */
#include <stdio.h>
#include <string.h>

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

static void check_plan(const uint8_t *plan, uint32_t plan_len, const uint8_t *written, const golden_step_t *steps, uint32_t step_count,
                       uint32_t scheme, uint32_t geometry_id, uint32_t product, uint32_t version,
                       uint32_t counter, uint32_t image_len) {
    const uint8_t *desc = chunky_plan_header_descriptor(plan);
    const uint8_t *body = chunky_signed_descriptor_body(desc);
    const uint8_t *at = plan + CHUNKY_PLAN_HEADER_LEN;
    const uint8_t *end = plan + plan_len;
    uint32_t n = 0;

    CHECK(memcmp(chunky_plan_header_magic(plan), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) == 0);
    CHECK(chunky_plan_header_format(plan) == 2u);
    CHECK(chunky_plan_header_geometry_id(plan) == geometry_id);
    CHECK(chunky_plan_header_step_count(plan) == step_count);
    CHECK(CHUNKY_PLAN_HEADER_LEN + chunky_plan_header_body_len(plan) == plan_len);

    CHECK(memcmp(chunky_descriptor_body_magic(body), "CKY2", 4u) == 0);
    CHECK(chunky_scheme_tag(chunky_descriptor_body_scheme(body)) == scheme);
    CHECK(chunky_descriptor_body_product_id(body) == product);
    CHECK(chunky_descriptor_body_version(body) == version);
    CHECK(chunky_descriptor_body_security_counter(body) == counter);
    CHECK(chunky_descriptor_body_image_len(body) == image_len);
    CHECK(chunky_descriptor_body_geometry_id(body) == geometry_id);
    CHECK(chunky_signed_descriptor_seal(desc) == desc + 64u);

    while (at < end && n < step_count) {
        const golden_step_t *want = &steps[n];
        const uint8_t *t = at + CHUNKY_STEP_TAG_LEN;
        chunky_step_tail_t tl;
        int rc;
        memset(&tl, 0, sizeof tl); /* only a WRITE sets write_data; gcc -O2 cannot see the tag decides it */
        rc = chunky_step_tail(at, &t, end, &tl);
        CHECK(rc == 0);
        if (rc != 0) {
            return;
        }
        CHECK(chunky_step_tag(at) == want->tag);
        switch (chunky_step_tag(at)) {
        case CHUNKY_STEP_ERASE:
            CHECK(chunky_step_erase_block(tl.head) == want->a);
            break;
        case CHUNKY_STEP_COPY:
            CHECK(chunky_step_copy_dst(tl.head) == want->a);
            CHECK(chunky_step_copy_src(tl.head) == want->b);
            CHECK(memcmp(chunky_step_copy_hash(tl.head), want->hash, 8u) == 0);
            break;
        case CHUNKY_STEP_WRITE:
            CHECK(chunky_step_write_dst(tl.head) == want->a);
            CHECK(chunky_step_write_data_count(tl.head) == want->b);
            CHECK(memcmp(chunky_step_write_hash(tl.head), want->hash, 8u) == 0);
            CHECK(memcmp(tl.write_data, written + want->data_at, want->b) == 0);
            break;
        default:
            CHECK(0);
            break;
        }
        at = t;
        n++;
    }
    CHECK(n == step_count);
    CHECK(at == end);
}

static void check_manifest(const uint8_t *m, uint32_t len, const golden_entry_t *entries, uint32_t entry_count,
                           uint32_t scheme, uint32_t geometry_id, uint32_t chunks) {
    const uint8_t *at = m + CHUNKY_MANIFEST_HEADER_LEN;
    const uint8_t *end = m + len;
    uint32_t n;

    CHECK(memcmp(chunky_manifest_header_magic(m), "CKM2", CHUNKY_MANIFEST_HEADER_MAGIC_COUNT) == 0);
    CHECK(chunky_manifest_header_format(m) == 2u);
    CHECK(chunky_scheme_tag(chunky_manifest_header_scheme(m)) == scheme);
    CHECK(chunky_manifest_header_geometry_id(m) == geometry_id);
    CHECK(chunky_manifest_header_chunks(m) == chunks);
    CHECK(2u * chunks == entry_count);

    for (n = 0; n < entry_count && at < end; n++) {
        const uint8_t *t = at + CHUNKY_CHUNK_ENTRY_TAG_LEN;
        chunky_chunk_entry_tail_t tl;
        int rc = chunky_chunk_entry_tail(at, &t, end, &tl);
        CHECK(rc == 0);
        if (rc != 0) {
            return;
        }
        CHECK(chunky_chunk_entry_tag(at) == entries[n].tag);
        if (entries[n].tag == CHUNKY_CHUNK_ENTRY_HASH) {
            CHECK(memcmp(chunky_chunk_entry_hash_hash(tl.head), entries[n].hash, 8u) == 0);
        }
        at = t;
    }
    CHECK(n == entry_count);
    CHECK(at == end);
}

int main(void) {
    check_plan(golden_signed_plan, sizeof golden_signed_plan, golden_signed_written, golden_signed_steps,
               sizeof golden_signed_steps / sizeof golden_signed_steps[0], CHUNKY_SCHEME_ED25519,
               GOLDEN_SIGNED_GEOMETRY_ID, GOLDEN_SIGNED_PRODUCT_ID, GOLDEN_SIGNED_VERSION,
               GOLDEN_SIGNED_SECURITY_COUNTER, GOLDEN_SIGNED_IMAGE_LEN);
    check_plan(golden_unsigned_plan, sizeof golden_unsigned_plan, golden_unsigned_written, golden_unsigned_steps,
               sizeof golden_unsigned_steps / sizeof golden_unsigned_steps[0], CHUNKY_SCHEME_UNSIGNED,
               GOLDEN_UNSIGNED_GEOMETRY_ID, GOLDEN_UNSIGNED_PRODUCT_ID, GOLDEN_UNSIGNED_VERSION,
               GOLDEN_UNSIGNED_SECURITY_COUNTER, GOLDEN_UNSIGNED_IMAGE_LEN);
    check_manifest(golden_signed_manifest, sizeof golden_signed_manifest, golden_signed_entries,
                   sizeof golden_signed_entries / sizeof golden_signed_entries[0], CHUNKY_SCHEME_ED25519,
                   GOLDEN_SIGNED_GEOMETRY_ID, GOLDEN_SIGNED_CHUNKS);
    check_manifest(golden_unsigned_manifest, sizeof golden_unsigned_manifest, golden_unsigned_entries,
                   sizeof golden_unsigned_entries / sizeof golden_unsigned_entries[0], CHUNKY_SCHEME_UNSIGNED,
                   GOLDEN_UNSIGNED_GEOMETRY_ID, GOLDEN_UNSIGNED_CHUNKS);

    if (failures != 0) {
        printf("chunky_format C reader: %d failures\n", failures);
        return 1;
    }
    printf("chunky_format C reader: golden vectors ok\n");
    return 0;
}
