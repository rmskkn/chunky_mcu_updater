/* format2_test.c: format2.c (lib/) against the vendored golden vectors
 * (rust/chunky_format/c/golden.h) and a handful of hand-built negative
 * cases. Run via tests/case_format2.sh, auto-discovered by run_all.sh's
 * case_*.sh glob; nothing in run_all.sh or the Makefile was changed to add
 * it.
 *
 * Positive path: both golden plans (signed, unsigned) and both golden
 * manifests decode and every field/step/entry matches golden.h's tables --
 * the same vectors upstream's own rust/chunky_format/c/test.c checks, so
 * this is also a cross-check that lib/format2.c reads the same bytes the
 * same way as the generated reader it is built on.
 *
 * Negative path: a representative case per refusal stage (SPEC.md 4.3,
 * 6.1), confirming format2.c actually stops where it says it does instead
 * of reading further into a bad buffer. */
#include <stdio.h>
#include <string.h>

#include "chunky_format.h"
#include "golden.h"
#include "format2.h"

static int failures;

#define CHECK(cond)                                           \
    do {                                                       \
        if (!(cond)) {                                         \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                        \
        }                                                       \
    } while (0)

#define CHECK_EQ_ERR(got, want)                                                            \
    do {                                                                                    \
        format2_err_t g_ = (got);                                                           \
        if (g_ != (want)) {                                                                 \
            printf("%s:%d: got %s, want %s\n", __FILE__, __LINE__,                         \
                   format2_err_str(g_), format2_err_str(want));                             \
            failures++;                                                                     \
        }                                                                                    \
    } while (0)

/* ---- positive path: plan header + descriptor ---- */

static void check_plan_begin(const uint8_t *plan, size_t plan_len, uint32_t version,
                             uint32_t counter, format2_err_t want) {
    const uint8_t *body = NULL;
    format2_err_t e = format2_plan_begin(plan, plan_len, GOLDEN_SIGNED_GEOMETRY_ID,
                                        GOLDEN_SIGNED_PRODUCT_ID, /* floor */ 0u,
                                        GOLDEN_SIGNED_IMAGE_LEN, &body);
    CHECK_EQ_ERR(e, want);
    if (e == FORMAT2_OK) {
        CHECK(body != NULL);
        CHECK(chunky_descriptor_body_version(body) == version);
        CHECK(chunky_descriptor_body_security_counter(body) == counter);
    }
}

/* ---- positive path: steps, against golden_step_t[] and golden_*_written ---- */

typedef struct {
    const golden_step_t *want;
    uint32_t count;
    const uint8_t *written;
    uint32_t seen;
} step_walk_ctx_t;

static int on_step(void *ctxp, const format2_step_t *s, uint32_t index) {
    step_walk_ctx_t *ctx = ctxp;
    const golden_step_t *w;

    CHECK(index == ctx->seen);
    CHECK(index < ctx->count);
    if (index >= ctx->count) {
        return -1;
    }
    w = &ctx->want[index];
    CHECK(s->op == w->tag);
    switch (s->op) {
    case CHUNKY_STEP_ERASE:
        CHECK(s->a == w->a);
        break;
    case CHUNKY_STEP_COPY:
        CHECK(s->a == w->a);
        CHECK(s->b == w->b);
        CHECK(s->hash != NULL && memcmp(s->hash, w->hash, 8u) == 0);
        break;
    case CHUNKY_STEP_WRITE:
        CHECK(s->a == w->a);
        CHECK(s->b == w->b);
        CHECK(s->hash != NULL && memcmp(s->hash, w->hash, 8u) == 0);
        CHECK(s->data != NULL && memcmp(s->data, ctx->written + w->data_at, w->b) == 0);
        break;
    default:
        CHECK(0);
        break;
    }
    ctx->seen++;
    return 0;
}

static void check_plan_walk(const uint8_t *plan, size_t plan_len, uint32_t step_count,
                            const golden_step_t *steps, uint32_t count, const uint8_t *written) {
    step_walk_ctx_t ctx;
    int rc;

    ctx.want = steps;
    ctx.count = count;
    ctx.written = written;
    ctx.seen = 0;
    /* chunks_per_block does not affect this vector's order check: its one
     * ERASE is block 0, position 0 regardless of the ratio. A real caller
     * passes its device geometry's erase / chunk. */
    rc = format2_plan_walk(plan, plan_len, step_count, /* chunks_per_block */ 4u, on_step, &ctx);
    CHECK(rc == 0);
    CHECK(ctx.seen == count);
}

/* ---- positive path: manifest header + entries ---- */

typedef struct {
    const golden_entry_t *want;
    uint32_t count;
    uint32_t seen;
} entry_walk_ctx_t;

static int on_entry(void *ctxp, const format2_entry_t *e, uint32_t index) {
    entry_walk_ctx_t *ctx = ctxp;
    const golden_entry_t *w;

    CHECK(index == ctx->seen);
    CHECK(index < ctx->count);
    if (index >= ctx->count) {
        return -1;
    }
    w = &ctx->want[index];
    CHECK(e->tag == w->tag);
    if (e->tag == CHUNKY_CHUNK_ENTRY_HASH) {
        CHECK(e->hash != NULL && memcmp(e->hash, w->hash, 8u) == 0);
    } else {
        CHECK(e->hash == NULL);
    }
    ctx->seen++;
    return 0;
}

static void check_manifest(const uint8_t *m, size_t len, const golden_entry_t *entries, uint32_t count) {
    uint16_t chunks = 0;
    entry_walk_ctx_t ctx;
    int rc;

    CHECK_EQ_ERR(format2_manifest_begin(m, len, GOLDEN_SIGNED_GEOMETRY_ID, &chunks), FORMAT2_OK);
    CHECK(2u * chunks == count);

    ctx.want = entries;
    ctx.count = count;
    ctx.seen = 0;
    rc = format2_manifest_walk(m, len, count, on_entry, &ctx);
    CHECK(rc == 0);
    CHECK(ctx.seen == count);
}

/* ---- negative path: one representative case per refusal stage ---- */

static void check_negative(void) {
    uint8_t buf[sizeof golden_unsigned_plan];
    const uint8_t *body;
    format2_err_t e;

    /* unmodified: sanity, must still pass (every mutation below starts from this) */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    CHECK_EQ_ERR(format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                                    GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, &body),
                FORMAT2_OK);

    /* stage: header refusal, plan magic */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    buf[0] ^= 0xFFu;
    CHECK_EQ_ERR(format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                                    GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, NULL),
                FORMAT2_EMAGIC);

    /* stage: header refusal, geometry id (checked against the caller's
     * value before anything sealed is touched -- no byte mutation needed) */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    e = format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID ^ 1u,
                           GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_EGEOMETRY);

    /* stage: header refusal, body length vs. the stream's actual length */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    e = format2_plan_begin(buf, sizeof buf - 1u, GOLDEN_UNSIGNED_GEOMETRY_ID,
                           GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_ELENGTH);

    /* stage: descriptor stage 1 refusal, image length field */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    e = format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                           GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN + 1u, NULL);
    CHECK_EQ_ERR(e, FORMAT2_ELENGTH);

    /* stage: the seal itself (a byte inside the 32-byte SHA-256 half of the
     * unsigned seal, which starts at plan offset 17 (descriptor) + 64 (body) = 81) */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    buf[81] ^= 0x01u;
    e = format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                           GOLDEN_UNSIGNED_PRODUCT_ID, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_ESIGNATURE);

    /* stage: decode/decide, product id (sealed field, checked by parameter
     * mismatch rather than a byte flip, since flipping it would just fail
     * the seal first -- this exercises the check that comes after) */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    e = format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                           GOLDEN_UNSIGNED_PRODUCT_ID ^ 1u, 0u, GOLDEN_UNSIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_EPRODUCT);

    /* stage: decode/decide, rollback floor */
    memcpy(buf, golden_unsigned_plan, sizeof buf);
    e = format2_plan_begin(buf, sizeof buf, GOLDEN_UNSIGNED_GEOMETRY_ID,
                           GOLDEN_UNSIGNED_PRODUCT_ID, GOLDEN_UNSIGNED_SECURITY_COUNTER + 1u,
                           GOLDEN_UNSIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_EROLLBACK);

    /* Ed25519 scheme: a well-formed descriptor this tree cannot seal-check.
     * Must come back as "unsupported", never as FORMAT2_OK. */
    e = format2_plan_begin(golden_signed_plan, sizeof golden_signed_plan, GOLDEN_SIGNED_GEOMETRY_ID,
                           GOLDEN_SIGNED_PRODUCT_ID, 0u, GOLDEN_SIGNED_IMAGE_LEN, NULL);
    CHECK_EQ_ERR(e, FORMAT2_ESCHEME_UNSUPPORTED);
}

/* ---- negative path: step order (SPEC.md 5.4) ----
 *
 * A minimal synthetic body: two WRITEs, chunk 3 then chunk 5 (position
 * strictly increases, SPEC.md 5.4), and the same two steps reversed
 * (position goes backwards). format2_plan_walk only reads the body, so no
 * valid header or seal is needed to exercise it. */
static int count_only(void *ctxp, const format2_step_t *s, uint32_t index) {
    (void)s;
    (void)index;
    if (ctxp != NULL) {
        (*(uint32_t *)ctxp)++;
    }
    return 0;
}

static void check_order_real(void) {
    static const uint8_t ok_body[] = {
        3, 3,0, 4,0, 0,0,0,0,0,0,0,0, 0xAA,0xBB,0xCC,0xDD,
        3, 5,0, 4,0, 0,0,0,0,0,0,0,0, 0x11,0x22,0x33,0x44,
    };
    static const uint8_t bad_body[] = {
        3, 5,0, 4,0, 0,0,0,0,0,0,0,0, 0x11,0x22,0x33,0x44,
        3, 3,0, 4,0, 0,0,0,0,0,0,0,0, 0xAA,0xBB,0xCC,0xDD,
    };
    uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof ok_body];
    uint32_t seen = 0;
    int rc;

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, ok_body, sizeof ok_body);
    rc = format2_plan_walk(buf, sizeof buf, 2u, 4u, count_only, &seen);
    CHECK(rc == 0);
    CHECK(seen == 2u);

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, bad_body, sizeof bad_body);
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 4u, count_only, &seen);
    CHECK(rc == -(int)FORMAT2_EORDER);
}

int main(void) {
    check_plan_begin(golden_unsigned_plan, sizeof golden_unsigned_plan,
                     GOLDEN_UNSIGNED_VERSION, GOLDEN_UNSIGNED_SECURITY_COUNTER, FORMAT2_OK);

    check_plan_walk(golden_signed_plan, sizeof golden_signed_plan, GOLDEN_SIGNED_STEP_COUNT,
                    golden_signed_steps, sizeof golden_signed_steps / sizeof golden_signed_steps[0],
                    golden_signed_written);
    check_plan_walk(golden_unsigned_plan, sizeof golden_unsigned_plan, GOLDEN_UNSIGNED_STEP_COUNT,
                    golden_unsigned_steps, sizeof golden_unsigned_steps / sizeof golden_unsigned_steps[0],
                    golden_unsigned_written);

    check_manifest(golden_signed_manifest, sizeof golden_signed_manifest,
                   golden_signed_entries, sizeof golden_signed_entries / sizeof golden_signed_entries[0]);
    check_manifest(golden_unsigned_manifest, sizeof golden_unsigned_manifest,
                   golden_unsigned_entries, sizeof golden_unsigned_entries / sizeof golden_unsigned_entries[0]);

    check_negative();
    check_order_real();

    if (failures != 0) {
        printf("format2_test: %d failures\n", failures);
        return 1;
    }
    printf("format2_test: ok\n");
    return 0;
}
