/* compress.c and lz4.c under ASan/UBSan (make test-compress). Buffers are
 * exact-size heap allocations so an over-read or overrun is a sanitizer report. */
#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
        printf("FAIL  %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); failures++; } } while (0)

static uint32_t rs = 0x9E3779B9u;
static uint32_t rnd(void)
{
    rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5;
    return rs;
}

static void fill_random(uint8_t *b) { for (int i = 0; i < SECTOR_SIZE; i++) b[i] = (uint8_t)rnd(); }

/* Firmware-like: a small vocabulary of 4-byte opcodes plus literal-pool runs. */
static void fill_realish(uint8_t *b, uint32_t seed)
{
    uint32_t save = rs;
    rs = seed | 1;
    uint8_t ops[24][4];
    for (int i = 0; i < 24; i++)
        for (int k = 0; k < 4; k++) ops[i][k] = (uint8_t)rnd();
    for (int i = 0; i + 4 <= SECTOR_SIZE; i += 4) {
        memcpy(b + i, ops[rnd() % 24], 4);
        b[i] = (uint8_t)(b[i] + (rnd() % 7));
    }
    for (int i = 64; i + 16 <= SECTOR_SIZE; i += 256 + (int)(rnd() % 128))
        for (int k = 0; k < 16; k++) b[i + k] = (uint8_t)(0x30 + k);
    rs = save;
}

static uint8_t *alloc(size_t n) { uint8_t *p = malloc(n ? n : 1); if (!p) abort(); return p; }

static size_t roundtrip(const char *name, const uint8_t *in, const uint8_t *dict, uint8_t *id)
{
    uint8_t *out = alloc(SECTOR_SIZE), *back = alloc(SECTOR_SIZE);
    size_t n = compress_sector(in, dict, out, id);
    CHECK(n > 0 && n <= SECTOR_SIZE, "%s: encoded length %zu", name, n);
    CHECK(*id == COMPRESS_RAW ? n == SECTOR_SIZE : n < SECTOR_SIZE,
          "%s: id %u with length %zu", name, *id, n);
    uint8_t *enc = alloc(n);                    /* exact size: decode may not read past it */
    memcpy(enc, out, n);
    memset(back, 0xA5, SECTOR_SIZE);
    int rc = decompress_sector(*id, enc, n, dict, back);
    CHECK(rc == 0, "%s: decode failed", name);
    CHECK(!memcmp(in, back, SECTOR_SIZE), "%s: round trip differs", name);
    free(enc); free(out); free(back);
    return n;
}

static void test_roundtrips(void)
{
    uint8_t *rnd_s = alloc(SECTOR_SIZE), *ff = alloc(SECTOR_SIZE), *zero = alloc(SECTOR_SIZE);
    uint8_t *real = alloc(SECTOR_SIZE), *other = alloc(SECTOR_SIZE);
    uint8_t c;

    fill_random(rnd_s);
    memset(ff, 0xFF, SECTOR_SIZE);
    memset(zero, 0x00, SECTOR_SIZE);
    fill_realish(real, 1);
    fill_random(other);

    for (int with_dict = 0; with_dict < 2; with_dict++) {
        const uint8_t *d = with_dict ? other : NULL;

        roundtrip("random", rnd_s, d, &c);
        CHECK(c == COMPRESS_RAW, "random input chose id %u, want RAW", c);

        size_t n = roundtrip("0xFF", ff, d, &c);
        CHECK(c != COMPRESS_RAW && n < 64, "0xFF: id %u, %zu B", c, n);
        n = roundtrip("0x00", zero, d, &c);
        CHECK(c != COMPRESS_RAW && n < 64, "0x00: id %u, %zu B", c, n);

        n = roundtrip("realish", real, d, &c);
        CHECK(c != COMPRESS_RAW && n < SECTOR_SIZE * 9 / 10, "realish: id %u, %zu B", c, n);
    }
    free(rnd_s); free(ff); free(zero); free(real); free(other);
}

/* Same code displaced by 4 bytes: what the dictionary is for. */
static void test_dict_shift(void)
{
    uint8_t *old = alloc(SECTOR_SIZE), *nw = alloc(SECTOR_SIZE);
    uint8_t *enc = alloc(SECTOR_SIZE), *back = alloc(SECTOR_SIZE), *wrong = alloc(SECTOR_SIZE);
    uint8_t c;

    fill_realish(old, 7);
    for (int i = 0; i < 4; i++) nw[i] = (uint8_t)rnd();
    memcpy(nw + 4, old, SECTOR_SIZE - 4);

    size_t n = roundtrip("shifted", nw, old, &c);
    CHECK(c == COMPRESS_LZ4_DICT, "shifted: id %u, want LZ4_DICT", c);
    CHECK(n < SECTOR_SIZE / 8, "shifted: %zu B, dictionary should make this tiny", n);

    /* wrong dictionary */
    c = 0;
    size_t m = compress_sector(nw, old, enc, &c);
    fill_random(wrong);
    memset(back, 0, SECTOR_SIZE);
    int rc = decompress_sector(c, enc, m, wrong, back);
    CHECK(rc != 0 || memcmp(back, nw, SECTOR_SIZE), "wrong dict reproduced the sector");

    CHECK(decompress_sector(COMPRESS_LZ4_DICT, enc, m, NULL, back) == -1, "DICT without dict accepted");

    free(old); free(nw); free(enc); free(back); free(wrong);
}

static void test_rejects(void)
{
    uint8_t *real = alloc(SECTOR_SIZE), *enc = alloc(SECTOR_SIZE), *out = alloc(SECTOR_SIZE);
    uint8_t *big = alloc(SECTOR_SIZE + 1), *dict = alloc(SECTOR_SIZE);
    uint8_t c;

    fill_realish(real, 3);
    fill_random(dict);
    size_t n = compress_sector(real, NULL, enc, &c);
    CHECK(c == COMPRESS_LZ4, "realish chose id %u", c);
    memset(big, 0, SECTOR_SIZE + 1);

    CHECK(decompress_sector(3, enc, n, NULL, out) == -1, "unknown id 3 accepted");
    CHECK(decompress_sector(255, enc, n, NULL, out) == -1, "unknown id 255 accepted");
    CHECK(decompress_sector(COMPRESS_LZ4, enc, 0, NULL, out) == -1, "enc_len 0 accepted");
    CHECK(decompress_sector(COMPRESS_RAW, enc, 0, NULL, out) == -1, "RAW enc_len 0 accepted");
    CHECK(decompress_sector(COMPRESS_LZ4, big, SECTOR_SIZE + 1, NULL, out) == -1, "enc_len > SECTOR_SIZE accepted");
    CHECK(decompress_sector(COMPRESS_RAW, enc, SECTOR_SIZE - 1, NULL, out) == -1, "short RAW accepted");

    /* every prefix of a valid stream */
    for (size_t k = 1; k < n; k++) {
        uint8_t *cut = alloc(k);
        memcpy(cut, enc, k);
        memset(out, 0xA5, SECTOR_SIZE);
        int rc = decompress_sector(COMPRESS_LZ4, cut, k, NULL, out);
        CHECK(rc != 0 || memcmp(out, real, SECTOR_SIZE), "prefix %zu/%zu decoded to the original", k, n);
        free(cut);
    }

    /* same for a DICT stream, then bit flips over both; a flip may decode to
     * the original (periodic data) and the hash judges that, not the decompressor */
    uint8_t *shifted = alloc(SECTOR_SIZE), *denc = alloc(SECTOR_SIZE);
    for (int i = 0; i < 4; i++) shifted[i] = (uint8_t)rnd();
    memcpy(shifted + 4, real, SECTOR_SIZE - 4);
    size_t dn = compress_sector(shifted, real, denc, &c);
    CHECK(c == COMPRESS_LZ4_DICT, "shifted chose id %u", c);
    for (size_t k = 1; k < dn; k++) {
        uint8_t *cut = alloc(k);
        memcpy(cut, denc, k);
        int rc = decompress_sector(COMPRESS_LZ4_DICT, cut, k, real, out);
        CHECK(rc != 0 || memcmp(out, shifted, SECTOR_SIZE), "dict prefix %zu/%zu decoded to the original", k, dn);
        free(cut);
    }

    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *src = pass ? denc : enc;
        size_t len = pass ? dn : n;
        uint8_t id = pass ? COMPRESS_LZ4_DICT : COMPRESS_LZ4;
        for (size_t i = 0; i < len; i++)
            for (int bit = 0; bit < 8; bit++) {
                uint8_t *f = alloc(len);
                memcpy(f, src, len);
                f[i] ^= (uint8_t)(1u << bit);
                memset(out, 0xA5, SECTOR_SIZE);
                (void)decompress_sector(id, f, len, pass ? real : NULL, out);
                free(f);
            }
    }

    free(real); free(enc); free(out); free(big); free(dict); free(shifted); free(denc);
}

/* A custom compressor through the registry: one repeated byte -> 2 bytes. */
static size_t fill_compress(const uint8_t *in, const uint8_t *dict, uint8_t *out)
{
    for (int i = 1; i < SECTOR_SIZE; i++)
        if (in[i] != in[0]) return 0;
    out[0] = in[0]; out[1] = 0;
    return 2;
}

static int fill_decompress(const uint8_t *enc, size_t enc_len, const uint8_t *dict, uint8_t *out)
{
    if (enc_len != 2) return -1;
    memset(out, enc[0], SECTOR_SIZE);
    return SECTOR_SIZE;
}

static void test_custom(void)
{
    static const compressor_t fill = { 3, "fill", 0, fill_compress, fill_decompress };
    static const compressor_t dup  = { COMPRESS_LZ4, "dup", 0, fill_compress, fill_decompress };
    static const compressor_t big  = { COMPRESS_MAX_ID + 1, "big", 0, fill_compress, fill_decompress };
    uint8_t *in = alloc(SECTOR_SIZE), *enc = alloc(SECTOR_SIZE), *out = alloc(SECTOR_SIZE);
    uint8_t id = 0;

    CHECK(compress_register(&dup) == -1, "duplicate id accepted");
    CHECK(compress_register(&big) == -1, "out-of-range id accepted");
    CHECK(compress_register(&fill) == 0, "register failed");
    CHECK(compress_register(&fill) == -1, "re-register accepted");

    memset(in, 0xFF, SECTOR_SIZE);
    size_t n = compress_sector(in, NULL, enc, &id);
    CHECK(id == 3 && n == 2, "custom compressor not chosen: id %u, %zu B", id, n);
    CHECK(decompress_sector(id, enc, n, NULL, out) == 0 && !memcmp(in, out, SECTOR_SIZE),
          "custom round trip");

    fill_random(in);
    compress_sector(in, NULL, enc, &id);
    CHECK(id == COMPRESS_RAW, "custom compressor chosen for random data");

    free(in); free(enc); free(out);
}

int main(void)
{
    test_roundtrips();
    test_dict_shift();
    test_rejects();
    test_custom();
    if (failures) { printf("compress_test: %d failure(s)\n", failures); return 1; }
    printf("PASS  compress_test (SECTOR_SIZE %d)\n", SECTOR_SIZE);
    return 0;
}
