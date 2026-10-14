/* The only file that includes liblz4. The MCU ships only the decompressors (~600 B). */
#include "lz4.h"
#include <lz4.h>
#include <lz4hc.h>

/* Capacity SECTOR_SIZE - 1: failing to fit means RAW wins. */
static size_t lz4_compress(const uint8_t *in, const uint8_t *dict, uint8_t *out)
{
    int n = LZ4_compress_HC((const char *)in, (char *)out, SECTOR_SIZE,
                            SECTOR_SIZE - 1, LZ4HC_CLEVEL_MAX);
    return n > 0 ? (size_t)n : 0;
}

static int lz4_decompress(const uint8_t *enc, size_t enc_len, const uint8_t *dict, uint8_t *out)
{
    return LZ4_decompress_safe((const char *)enc, (char *)out, (int)enc_len, SECTOR_SIZE);
}

static size_t lz4_dict_compress(const uint8_t *in, const uint8_t *dict, uint8_t *out)
{
    LZ4_streamHC_t *s = LZ4_createStreamHC();
    if (!s) return 0;
    LZ4_resetStreamHC_fast(s, LZ4HC_CLEVEL_MAX);
    LZ4_loadDictHC(s, (const char *)dict, SECTOR_SIZE);
    int n = LZ4_compress_HC_continue(s, (const char *)in, (char *)out,
                                     SECTOR_SIZE, SECTOR_SIZE - 1);
    LZ4_freeStreamHC(s);
    return n > 0 ? (size_t)n : 0;
}

static int lz4_dict_decompress(const uint8_t *enc, size_t enc_len, const uint8_t *dict, uint8_t *out)
{
    return LZ4_decompress_safe_usingDict((const char *)enc, (char *)out, (int)enc_len,
                                         SECTOR_SIZE, (const char *)dict, SECTOR_SIZE);
}

const compressor_t lz4_compressor = {
    COMPRESS_LZ4, "lz4", 0, lz4_compress, lz4_decompress
};
const compressor_t lz4_dict_compressor = {
    COMPRESS_LZ4_DICT, "lz4+dict", 1, lz4_dict_compress, lz4_dict_decompress
};
