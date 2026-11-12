/* The generated C reader (chunky_format.h) against the golden vectors (golden.h), signed and unsigned: it walks
 * the whole plan and the whole manifest and compares every field with what the planner and the manifest say
 * (the tables of golden.h). Then the same bytes cut short, one prefix at a time, each in a buffer of exactly its
 * length: every view refuses them, and so does a walk of the whole plan or manifest. `make vectors`. */
#include <stdio.h>
#include <stdlib.h>
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
    chunky_plan_header_view_t h;
    chunky_signed_descriptor_view_t desc;
    chunky_descriptor_body_view_t body;
    const uint8_t *at;
    const uint8_t *end = plan + plan_len;
    uint32_t n = 0;

    CHECK(chunky_plan_header_view(plan, plan_len, &h) == 0);
    if (h.p == NULL) {
        return;
    }
    CHECK(h.len == CHUNKY_PLAN_HEADER_LEN);
    desc = chunky_plan_header_descriptor(h);
    CHECK(desc.len == CHUNKY_SIGNED_DESCRIPTOR_LEN);
    CHECK(chunky_descriptor_body_view(chunky_signed_descriptor_body(desc), CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT, &body) == 0);
    if (body.p == NULL) {
        return;
    }

    CHECK(memcmp(chunky_plan_header_magic(h), "CKP2", CHUNKY_PLAN_HEADER_MAGIC_COUNT) == 0);
    CHECK(chunky_plan_header_format(h) == 2u);
    CHECK(chunky_plan_header_geometry_id(h) == geometry_id);
    CHECK(chunky_plan_header_step_count(h) == step_count);
    CHECK(CHUNKY_PLAN_HEADER_LEN + chunky_plan_header_body_len(h) == plan_len);

    CHECK(memcmp(chunky_descriptor_body_magic(body), "CKY2", 4u) == 0);
    CHECK(chunky_scheme_tag(chunky_descriptor_body_scheme(body)) == scheme);
    CHECK(chunky_descriptor_body_product_id(body) == product);
    CHECK(chunky_descriptor_body_version(body) == version);
    CHECK(chunky_descriptor_body_security_counter(body) == counter);
    CHECK(chunky_descriptor_body_image_len(body) == image_len);
    CHECK(chunky_descriptor_body_geometry_id(body) == geometry_id);
    CHECK(chunky_signed_descriptor_seal(desc) == desc.p + 64u);

    at = plan + h.len;
    while (at < end && n < step_count) {
        const golden_step_t *want = &steps[n];
        chunky_step_view_t s;
        const chunky_step_erase_view_t *erase;
        const chunky_step_copy_view_t *copy;
        const chunky_step_write_view_t *write;
        int rc = chunky_step_view(at, (size_t)(end - at), &s);
        CHECK(rc == 0);
        if (rc != 0) {
            return;
        }
        CHECK(chunky_step_tag(&s) == want->tag);
        if (chunky_step_as_erase(&s, &erase) == 0) {
            CHECK(chunky_step_erase_block(erase) == want->a);
        } else if (chunky_step_as_copy(&s, &copy) == 0) {
            CHECK(chunky_step_copy_dst(copy) == want->a);
            CHECK(chunky_step_copy_src(copy) == want->b);
            CHECK(memcmp(chunky_step_copy_hash(copy), want->hash, 8u) == 0);
        } else if (chunky_step_as_write(&s, &write) == 0) {
            CHECK(chunky_step_write_dst(write) == want->a);
            CHECK(chunky_step_write_data_count(write) == want->b);
            CHECK(memcmp(chunky_step_write_hash(write), want->hash, 8u) == 0);
            CHECK(memcmp(chunky_step_write_data(write), written + want->data_at, want->b) == 0);
        } else {
            CHECK(0);
        }
        at += s.len;
        n++;
    }
    CHECK(n == step_count);
    CHECK(at == end);
}

static void check_manifest(const uint8_t *m, uint32_t len, const golden_entry_t *entries, uint32_t entry_count,
                           uint32_t scheme, uint32_t geometry_id, uint32_t chunks) {
    chunky_manifest_header_view_t h;
    const uint8_t *at;
    const uint8_t *end = m + len;
    uint32_t n;

    CHECK(chunky_manifest_header_view(m, len, &h) == 0);
    if (h.p == NULL) {
        return;
    }
    CHECK(memcmp(chunky_manifest_header_magic(h), "CKM2", CHUNKY_MANIFEST_HEADER_MAGIC_COUNT) == 0);
    CHECK(chunky_manifest_header_format(h) == 2u);
    CHECK(chunky_scheme_tag(chunky_manifest_header_scheme(h)) == scheme);
    CHECK(chunky_manifest_header_geometry_id(h) == geometry_id);
    CHECK(chunky_manifest_header_chunks(h) == chunks);
    CHECK(2u * chunks == entry_count);

    at = m + h.len;
    for (n = 0; n < entry_count && at < end; n++) {
        chunky_chunk_entry_view_t e;
        const chunky_chunk_entry_hash_view_t *hash;
        int rc = chunky_chunk_entry_view(at, (size_t)(end - at), &e);
        CHECK(rc == 0);
        if (rc != 0) {
            return;
        }
        CHECK(chunky_chunk_entry_tag(&e) == entries[n].tag);
        if (chunky_chunk_entry_as_hash(&e, &hash) == 0) {
            CHECK(entries[n].tag == CHUNKY_CHUNK_ENTRY_HASH);
            CHECK(memcmp(chunky_chunk_entry_hash_hash(hash), entries[n].hash, 8u) == 0);
        } else {
            CHECK(entries[n].tag != CHUNKY_CHUNK_ENTRY_HASH);
        }
        at += e.len;
    }
    CHECK(n == entry_count);
    CHECK(at == end);
}

/* 0 when the len bytes at p are a whole plan: its header, step_count steps and nothing after them */
static int walk_plan(const uint8_t *p, size_t len) {
    chunky_plan_header_view_t h;
    chunky_signed_descriptor_view_t desc;
    chunky_descriptor_body_view_t body;
    size_t at;
    uint32_t n;

    if (chunky_plan_header_view(p, len, &h) != 0) {
        return -1;
    }
    desc = chunky_plan_header_descriptor(h);
    if (chunky_descriptor_body_view(chunky_signed_descriptor_body(desc), CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT, &body) != 0) {
        return -1;
    }
    at = h.len;
    for (n = 0; n < chunky_plan_header_step_count(h); n++) {
        chunky_step_view_t s;
        if (chunky_step_view(p + at, len - at, &s) != 0) {
            return -1;
        }
        at += s.len;
    }
    return at == len ? 0 : -1;
}

/* 0 when the len bytes at m are a whole manifest: its header, two entries per chunk and nothing after them */
static int walk_manifest(const uint8_t *m, size_t len) {
    chunky_manifest_header_view_t h;
    size_t at;
    uint32_t n;

    if (chunky_manifest_header_view(m, len, &h) != 0) {
        return -1;
    }
    at = h.len;
    for (n = 0; n < 2u * (uint32_t)chunky_manifest_header_chunks(h); n++) {
        chunky_chunk_entry_view_t e;
        if (chunky_chunk_entry_view(m + at, len - at, &e) != 0) {
            return -1;
        }
        at += e.len;
    }
    return at == len ? 0 : -1;
}

/* the first len bytes of src in a buffer of exactly len bytes, so a read past it is out of bounds (NULL for 0) */
static uint8_t *exact(const uint8_t *src, size_t len) {
    uint8_t *b;
    if (len == 0u) {
        return NULL;
    }
    b = malloc(len);
    if (b == NULL) {
        printf("out of memory\n");
        exit(1);
    }
    memcpy(b, src, len);
    return b;
}

/* every prefix of a plan, and of each of its steps, is refused; so are NULL, an unknown scheme, an unknown step
   tag and an ERASE tagged as another op */
static void check_short_plan(const uint8_t *plan, size_t plan_len) {
    chunky_plan_header_view_t h;
    chunky_signed_descriptor_view_t desc;
    chunky_descriptor_body_view_t body;
    chunky_step_view_t s;
    uint8_t *b;
    size_t len;
    size_t at;

    b = exact(plan, plan_len);
    CHECK(walk_plan(b, plan_len) == 0);
    free(b);
    for (len = 0u; len < plan_len; len++) {
        b = exact(plan, len);
        CHECK(walk_plan(b, len) != 0);
        CHECK(chunky_plan_header_view(b, len, &h) == (len < CHUNKY_PLAN_HEADER_LEN ? -1 : 0));
        free(b);
    }
    CHECK(chunky_plan_header_view(NULL, plan_len, &h) != 0);
    CHECK(h.p == NULL && h.len == 0u);
    CHECK(chunky_signed_descriptor_view(NULL, CHUNKY_SIGNED_DESCRIPTOR_LEN, &desc) != 0);
    CHECK(chunky_descriptor_body_view(NULL, CHUNKY_DESCRIPTOR_BODY_LEN, &body) != 0);

    /* a scheme the descriptor body does not know */
    b = exact(plan, plan_len);
    b[CHUNKY_PLAN_HEADER_DESCRIPTOR_OFFSET + CHUNKY_DESCRIPTOR_BODY_SCHEME_OFFSET] = 0u;
    CHECK(chunky_plan_header_view(b, plan_len, &h) == 0);
    desc = chunky_plan_header_descriptor(h);
    CHECK(chunky_descriptor_body_view(chunky_signed_descriptor_body(desc), CHUNKY_SIGNED_DESCRIPTOR_BODY_COUNT, &body) != 0);
    CHECK(walk_plan(b, plan_len) != 0);
    free(b);

    /* each step alone, cut at every length, and with an unknown tag */
    at = CHUNKY_PLAN_HEADER_LEN;
    while (at < plan_len) {
        size_t step_len;
        if (chunky_step_view(plan + at, plan_len - at, &s) != 0) {
            CHECK(0);
            return;
        }
        step_len = s.len;
        for (len = 0u; len < step_len; len++) {
            b = exact(plan + at, len);
            CHECK(chunky_step_view(b, len, &s) != 0);
            CHECK(s.p == NULL && s.len == 0u);
            free(b);
        }
        b = exact(plan + at, step_len);
        CHECK(chunky_step_view(b, step_len, &s) == 0 && s.len == step_len);
        b[0] = 0u;
        CHECK(chunky_step_view(b, step_len, &s) != 0);
        b[0] = 4u;
        CHECK(chunky_step_view(b, step_len, &s) != 0);
        /* an ERASE's 3 bytes tagged as a COPY or a WRITE are too short for their head */
        if (plan[at] == CHUNKY_STEP_ERASE) {
            b[0] = CHUNKY_STEP_COPY;
            CHECK(chunky_step_view(b, step_len, &s) != 0);
            b[0] = CHUNKY_STEP_WRITE;
            CHECK(chunky_step_view(b, step_len, &s) != 0);
        }
        free(b);
        at += step_len;
    }
    CHECK(chunky_step_view(NULL, 16u, &s) != 0);
}

/* every prefix of a manifest is refused; so is NULL, an unknown scheme and an unknown entry tag */
static void check_short_manifest(const uint8_t *m, size_t m_len) {
    chunky_manifest_header_view_t h;
    chunky_chunk_entry_view_t e;
    uint8_t *b;
    size_t len;

    b = exact(m, m_len);
    CHECK(walk_manifest(b, m_len) == 0);
    b[CHUNKY_MANIFEST_HEADER_SCHEME_OFFSET] = 3u; /* a scheme the header does not know */
    CHECK(chunky_manifest_header_view(b, m_len, &h) != 0);
    CHECK(walk_manifest(b, m_len) != 0);
    memcpy(b, m, m_len);
    b[CHUNKY_MANIFEST_HEADER_LEN] = 3u;
    CHECK(chunky_chunk_entry_view(b + CHUNKY_MANIFEST_HEADER_LEN, m_len - CHUNKY_MANIFEST_HEADER_LEN, &e) != 0);
    CHECK(walk_manifest(b, m_len) != 0);
    free(b);
    for (len = 0u; len < m_len; len++) {
        b = exact(m, len);
        CHECK(walk_manifest(b, len) != 0);
        CHECK(chunky_manifest_header_view(b, len, &h) == (len < CHUNKY_MANIFEST_HEADER_LEN ? -1 : 0));
        free(b);
    }
    CHECK(chunky_manifest_header_view(NULL, m_len, &h) != 0);
    CHECK(chunky_chunk_entry_view(NULL, 9u, &e) != 0);
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

    check_short_plan(golden_signed_plan, sizeof golden_signed_plan);
    check_short_plan(golden_unsigned_plan, sizeof golden_unsigned_plan);
    check_short_manifest(golden_signed_manifest, sizeof golden_signed_manifest);
    check_short_manifest(golden_unsigned_manifest, sizeof golden_unsigned_manifest);

    if (failures != 0) {
        printf("chunky_format C reader: %d failures\n", failures);
        return 1;
    }
    printf("chunky_format C reader: golden vectors ok, every shorter buffer refused\n");
    return 0;
}
