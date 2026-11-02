/* format2_test.c: format2.c (lib/) against the vendored golden vectors
 * (rust/chunky_format/c/golden.h) and a set of hand-built cases, including
 * chunk-hash verification (SPEC.md 6.2: a WRITE's data and a COPY's source
 * must have the hash the step names). Run via tests/case_format2.sh,
 * auto-discovered by run_all.sh's case_*.sh glob; nothing in run_all.sh or
 * the Makefile was changed to add it.
 *
 * Why the golden-vector plan walk is split in two:
 *
 *   - check_plan_walk_prefix() walks only the ERASE + first WRITE of each
 *     golden plan against real data: format2_chunk_hash() is checked against
 *     a genuine WRITE hash from the vectors, independently confirmed while
 *     writing this test (node:crypto: sha256(golden_unsigned_written[0:256])
 *     [:8] == golden_unsigned_steps[1].hash, and the same for the signed
 *     vector), not just against format2_chunk_hash() itself.
 *   - check_plan_walk_noactive_refuses() walks the FULL golden plan with no
 *     read_active callback and confirms it gets exactly as far as the ERASE
 *     and WRITE before refusing at the first COPY with FORMAT2_ENOACTIVE --
 *     the vendored vectors have no ACTIVE bytes to check a COPY source
 *     against (only golden_*_written, the WRITE side), so this is also the
 *     only honest way to exercise a COPY step against the real vectors:
 *     confirming the gap closes by refusing, not by guessing.
 *
 * check_hash_checks() then covers a COPY's source hash end to end with a
 * small synthetic plan and a mock ACTIVE, using hashes computed independently
 * (node:crypto, not format2_chunk_hash) so a bug in format2_chunk_hash
 * itself would not go unnoticed. */
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

/* The golden fixture's chunk size: GOLDEN_*_CHUNKS (16) chunks fill
 * GOLDEN_*_IMAGE_LEN + 128 (the descriptor) = 4096 bytes, so chunk = 256.
 * Not a golden.h constant; derived once here and asserted below. */
#define GOLDEN_CHUNK_LEN 256u

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

/* ---- steps: structural fields against golden_step_t[] and golden_*_written ---- */

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

/* Byte length of this fixture's first two steps' worth of plan body: one
 * ERASE (1 + 2 bytes) then one WRITE of GOLDEN_CHUNK_LEN bytes (1 + 12 +
 * GOLDEN_CHUNK_LEN). format2_plan_walk requires len to match step_count
 * exactly (SPEC.md 5.3: nothing after the declared body is read as steps),
 * so a genuine prefix walk needs the buffer truncated to match -- unlike
 * passing the whole plan with a smaller step_count, which would leave
 * trailing bytes and (correctly) fail with FORMAT2_ELENGTH. */
#define GOLDEN_PREFIX_LEN (CHUNKY_PLAN_HEADER_LEN + (1u + 2u) + (1u + 12u + GOLDEN_CHUNK_LEN))

/* Walks just the first two steps (ERASE then its first WRITE, true of both
 * golden plans) with no ACTIVE needed: real data, real hash check. */
static void check_plan_walk_prefix(const uint8_t *plan, const golden_step_t *steps, const uint8_t *written) {
    step_walk_ctx_t ctx;
    int rc;

    ctx.want = steps;
    ctx.count = 2u;
    ctx.written = written;
    ctx.seen = 0;
    /* blocks=1 is a placeholder, not the device's real erase-block count:
     * the one ERASE step here is block 0, which is in range for any
     * blocks >= 1, and nothing after it in this prefix needs the real
     * value. chunks/chunk_len are the real fixture geometry (16 chunks of
     * 256 bytes, GOLDEN_UNSIGNED_CHUNKS). */
    rc = format2_plan_walk(plan, GOLDEN_PREFIX_LEN, 2u, /* blocks */ 1u,
                           GOLDEN_UNSIGNED_CHUNKS, GOLDEN_CHUNK_LEN,
                           /* read_active */ NULL, NULL, /* scratch */ NULL,
                           on_step, &ctx);
    CHECK(rc == 0);
    CHECK(ctx.seen == 2u);
}

/* Walks the FULL golden plan with no read_active: must get exactly through
 * first_copy_index steps (the ERASE and WRITE ahead of the first COPY),
 * then refuse with FORMAT2_ENOACTIVE rather than silently accepting an
 * unchecked COPY. */
static void check_plan_walk_noactive_refuses(const uint8_t *plan, size_t plan_len,
                                             uint32_t step_count, const golden_step_t *steps,
                                             const uint8_t *written, uint32_t first_copy_index) {
    step_walk_ctx_t ctx;
    int rc;

    ctx.want = steps;
    ctx.count = step_count;
    ctx.written = written;
    ctx.seen = 0;
    rc = format2_plan_walk(plan, plan_len, step_count, /* blocks */ 1u,
                           GOLDEN_UNSIGNED_CHUNKS, GOLDEN_CHUNK_LEN,
                           /* read_active */ NULL, NULL, /* scratch */ NULL,
                           on_step, &ctx);
    CHECK(rc == -(int)FORMAT2_ENOACTIVE);
    CHECK(ctx.seen == first_copy_index);
}

/* ---- positive path: manifest header + entries (no hash checking: the
 * manifest is unsigned and untrusted by design, SPEC.md 7.2) ---- */

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

/* ---- negative path: one representative case per format2_plan_begin refusal stage ---- */

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
 * Two WRITEs, chunk 3 then chunk 5 (position strictly increases) and the
 * same two steps reversed (position goes backwards). Real, independently
 * computed hashes (node:crypto), not zeros: with chunk-hash checking now
 * in format2_plan_walk, a wrong hash would stop the walk before the order
 * check ever ran, which is not what this case means to exercise. */
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
        /* WRITE dst=3, count=4, hash(0xAA,0xBB,0xCC,0xDD), data */
        3, 3,0, 4,0, 0x8d,0x70,0xd6,0x91,0xc8,0x22,0xd5,0x56, 0xAA,0xBB,0xCC,0xDD,
        /* WRITE dst=5, count=4, hash(0x11,0x22,0x33,0x44), data */
        3, 5,0, 4,0, 0x1a,0x83,0x5e,0xd8,0x73,0x4f,0x86,0x35, 0x11,0x22,0x33,0x44,
    };
    static const uint8_t bad_body[] = {
        3, 5,0, 4,0, 0x1a,0x83,0x5e,0xd8,0x73,0x4f,0x86,0x35, 0x11,0x22,0x33,0x44,
        3, 3,0, 4,0, 0x8d,0x70,0xd6,0x91,0xc8,0x22,0xd5,0x56, 0xAA,0xBB,0xCC,0xDD,
    };
    uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof ok_body];
    uint32_t seen = 0;
    int rc;

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, ok_body, sizeof ok_body);
    rc = format2_plan_walk(buf, sizeof buf, 2u, /* blocks */ 1u, /* chunks */ 8u, /* chunk_len */ 4u,
                           NULL, NULL, NULL, count_only, &seen);
    CHECK(rc == 0);
    CHECK(seen == 2u);

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, bad_body, sizeof bad_body);
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 1u, 8u, 4u, NULL, NULL, NULL, count_only, &seen);
    CHECK(rc == -(int)FORMAT2_EORDER);
}

/* ---- chunk hash verification (SPEC.md 6.2: DataMismatch, SourceMismatch,
 * SourceUnreadable) and the "no ACTIVE, no silent pass" rule ----
 *
 * One synthetic plan body: COPY(dst=2, src=3) then WRITE(dst=5). Both
 * hashes below are independently computed (node:crypto), not via
 * format2_chunk_hash, from:
 *   "active" chunk 3 = DE AD BE EF   -> hash 5f78c33274e43fa9
 *   WRITE's data     = 99 88 77 66   -> hash dc5db77b8b827c99 */
static const uint8_t hash_body[] = {
    2, 2,0, 3,0, 0x5f,0x78,0xc3,0x32,0x74,0xe4,0x3f,0xa9,
    3, 5,0, 4,0, 0xdc,0x5d,0xb7,0x7b,0x8b,0x82,0x7c,0x99, 0x99,0x88,0x77,0x66,
};
#define HASH_BODY_CHUNK_LEN 4u

typedef struct {
    const uint8_t *data; /* HASH_BODY_CHUNK_LEN bytes, or NULL to fail every read */
} mock_active_t;

static int mock_read_active(void *ctxp, uint16_t chunk_index, uint8_t *buf, uint16_t chunk_len) {
    const mock_active_t *m = ctxp;
    (void)chunk_index; /* this mock only ever serves one chunk, whichever is asked for */
    if (m->data == NULL) {
        return -1; /* "unreadable", e.g. an ECC fault */
    }
    memcpy(buf, m->data, chunk_len);
    return 0;
}

static void check_hash_checks(void) {
    static const uint8_t active_chunk3[HASH_BODY_CHUNK_LEN] = {0xDE, 0xAD, 0xBE, 0xEF};
    static const uint8_t wrong_chunk3[HASH_BODY_CHUNK_LEN] = {0xDE, 0xAD, 0xBE, 0xFF};
    uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof hash_body];
    uint8_t scratch[HASH_BODY_CHUNK_LEN];
    mock_active_t active;
    uint32_t seen;
    int rc;

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, hash_body, sizeof hash_body);

    /* 1. correct source and correct data: both steps delivered */
    active.data = active_chunk3;
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 1u, 8u, HASH_BODY_CHUNK_LEN,
                           mock_read_active, &active, scratch, count_only, &seen);
    CHECK(rc == 0);
    CHECK(seen == 2u);

    /* 2. wrong source bytes: SourceMismatch, before the WRITE is ever reached */
    active.data = wrong_chunk3;
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 1u, 8u, HASH_BODY_CHUNK_LEN,
                           mock_read_active, &active, scratch, count_only, &seen);
    CHECK(rc == -(int)FORMAT2_ESOURCE);
    CHECK(seen == 0u);

    /* 3. no read_active at all: refuses outright, never silently passes */
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 1u, 8u, HASH_BODY_CHUNK_LEN,
                           NULL, NULL, scratch, count_only, &seen);
    CHECK(rc == -(int)FORMAT2_ENOACTIVE);
    CHECK(seen == 0u);

    /* 4. the source is unreadable (ECC fault stand-in) */
    active.data = NULL;
    seen = 0;
    rc = format2_plan_walk(buf, sizeof buf, 2u, 1u, 8u, HASH_BODY_CHUNK_LEN,
                           mock_read_active, &active, scratch, count_only, &seen);
    CHECK(rc == -(int)FORMAT2_ESOURCE_UNREADABLE);
    CHECK(seen == 0u);

    /* 5. correct source, but the WRITE's own hash is wrong: DataMismatch,
     * after the COPY (which is still correct) has already been delivered */
    {
        uint8_t bad[sizeof buf];
        memcpy(bad, buf, sizeof buf);
        bad[CHUNKY_PLAN_HEADER_LEN + 13u + 5u] ^= 0x01u; /* one byte of the WRITE's hash */
        active.data = active_chunk3;
        seen = 0;
        rc = format2_plan_walk(bad, sizeof bad, 2u, 1u, 8u, HASH_BODY_CHUNK_LEN,
                               mock_read_active, &active, scratch, count_only, &seen);
        CHECK(rc == -(int)FORMAT2_EDATA);
        CHECK(seen == 1u);
    }
}

/* ---- WRITE byte count must equal the geometry's chunk size (SPEC.md 6.2.4) ---- */

static void check_datalen(void) {
    /* count=3 in the head, but chunk_len (the geometry's real chunk size)
     * is 4: chunky_step_tail still frames it (it trusts the head's own
     * count field), format2_plan_walk is the one that refuses it. */
    static const uint8_t body[] = {
        3, 0,0, 3,0, 0,0,0,0,0,0,0,0, 0x01,0x02,0x03,
    };
    uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof body];
    int rc;

    memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
    memcpy(buf + CHUNKY_PLAN_HEADER_LEN, body, sizeof body);
    rc = format2_plan_walk(buf, sizeof buf, 1u, 1u, 8u, /* chunk_len */ 4u,
                           NULL, NULL, NULL, count_only, NULL);
    CHECK(rc == -(int)FORMAT2_EDATALEN);
}

/* ---- index range checks (SPEC.md 6.2.5: block < blocks, dst/src < chunks) ---- */

static void check_range(void) {
    int rc;

    /* ERASE block 5, but blocks = 1 */
    {
        static const uint8_t body[] = {1, 5,0};
        uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof body];
        memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
        memcpy(buf + CHUNKY_PLAN_HEADER_LEN, body, sizeof body);
        rc = format2_plan_walk(buf, sizeof buf, 1u, /* blocks */ 1u, 8u, 4u,
                               NULL, NULL, NULL, count_only, NULL);
        CHECK(rc == -(int)FORMAT2_ERANGE);
    }
    /* COPY src 10, but chunks = 4 */
    {
        static const uint8_t body[] = {2, 0,0, 10,0, 0,0,0,0,0,0,0,0};
        uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof body];
        memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
        memcpy(buf + CHUNKY_PLAN_HEADER_LEN, body, sizeof body);
        rc = format2_plan_walk(buf, sizeof buf, 1u, 1u, /* chunks */ 4u, 4u,
                               NULL, NULL, NULL, count_only, NULL);
        CHECK(rc == -(int)FORMAT2_ERANGE);
    }
    /* WRITE dst 10, but chunks = 4 (count is correct, 4, so EDATALEN does not fire first) */
    {
        static const uint8_t body[] = {3, 10,0, 4,0, 0,0,0,0,0,0,0,0, 0,0,0,0};
        uint8_t buf[CHUNKY_PLAN_HEADER_LEN + sizeof body];
        memset(buf, 0, CHUNKY_PLAN_HEADER_LEN);
        memcpy(buf + CHUNKY_PLAN_HEADER_LEN, body, sizeof body);
        rc = format2_plan_walk(buf, sizeof buf, 1u, 1u, /* chunks */ 4u, 4u,
                               NULL, NULL, NULL, count_only, NULL);
        CHECK(rc == -(int)FORMAT2_ERANGE);
    }
}

int main(void) {
    /* sanity: the derived GOLDEN_CHUNK_LEN really is this fixture's chunk size */
    CHECK((uint32_t)GOLDEN_CHUNK_LEN * GOLDEN_UNSIGNED_CHUNKS == GOLDEN_UNSIGNED_IMAGE_LEN + 128u);

    check_plan_begin(golden_unsigned_plan, sizeof golden_unsigned_plan,
                     GOLDEN_UNSIGNED_VERSION, GOLDEN_UNSIGNED_SECURITY_COUNTER, FORMAT2_OK);

    /* real data, real hash check, no ACTIVE needed (ERASE + first WRITE only) */
    check_plan_walk_prefix(golden_signed_plan, golden_signed_steps, golden_signed_written);
    check_plan_walk_prefix(golden_unsigned_plan, golden_unsigned_steps, golden_unsigned_written);

    /* real data, full plan, no read_active: refuses at the first COPY (index 2) */
    check_plan_walk_noactive_refuses(golden_signed_plan, sizeof golden_signed_plan,
                                     GOLDEN_SIGNED_STEP_COUNT, golden_signed_steps,
                                     golden_signed_written, 2u);
    check_plan_walk_noactive_refuses(golden_unsigned_plan, sizeof golden_unsigned_plan,
                                     GOLDEN_UNSIGNED_STEP_COUNT, golden_unsigned_steps,
                                     golden_unsigned_written, 2u);

    check_manifest(golden_signed_manifest, sizeof golden_signed_manifest,
                   golden_signed_entries, sizeof golden_signed_entries / sizeof golden_signed_entries[0]);
    check_manifest(golden_unsigned_manifest, sizeof golden_unsigned_manifest,
                   golden_unsigned_entries, sizeof golden_unsigned_entries / sizeof golden_unsigned_entries[0]);

    check_negative();
    check_order_real();
    check_hash_checks();
    check_datalen();
    check_range();

    if (failures != 0) {
        printf("format2_test: %d failures\n", failures);
        return 1;
    }
    printf("format2_test: ok\n");
    return 0;
}
