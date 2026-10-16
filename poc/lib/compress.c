#include "compress.h"
#include "lz4.h"
#include <string.h>

static const compressor_t *reg[COMPRESS_MAX_ID + 1];

static void builtins(void)
{
    if (!reg[COMPRESS_LZ4]) {
        reg[COMPRESS_LZ4]      = &lz4_compressor;
        reg[COMPRESS_LZ4_DICT] = &lz4_dict_compressor;
    }
}

int compress_register(const compressor_t *c)
{
    builtins();
    if (c->id == COMPRESS_RAW || c->id > COMPRESS_MAX_ID || reg[c->id]) return -1;
    reg[c->id] = c;
    return 0;
}

size_t compress_sector(const uint8_t in[SECTOR_SIZE], const uint8_t *dict,
                       uint8_t out[SECTOR_SIZE], uint8_t *id)
{
    uint8_t tmp[SECTOR_SIZE];
    size_t best = 0;

    builtins();
    for (int i = 1; i <= COMPRESS_MAX_ID; i++) {
        const compressor_t *c = reg[i];
        if (!c || (c->uses_dict && !dict)) continue;
        size_t n = c->compress(in, c->uses_dict ? dict : NULL, best ? tmp : out);
        if (n == 0 || n >= SECTOR_SIZE) continue;
        if (best && n < best) memcpy(out, tmp, n);
        if (!best || n < best) { best = n; *id = c->id; }
    }
    if (!best) {
        memcpy(out, in, SECTOR_SIZE);
        *id = COMPRESS_RAW;
        return SECTOR_SIZE;
    }
    return best;
}

int decompress_sector(uint8_t id, const uint8_t *enc, size_t enc_len,
                      const uint8_t *dict, uint8_t out[SECTOR_SIZE])
{
    if (enc_len == 0 || enc_len > SECTOR_SIZE) return -1;

    if (id == COMPRESS_RAW) {
        if (enc_len != SECTOR_SIZE) return -1;
        memcpy(out, enc, SECTOR_SIZE);
        return 0;
    }
    builtins();
    if (id > COMPRESS_MAX_ID || !reg[id]) return -1;
    if (reg[id]->uses_dict && !dict) return -1;
    return reg[id]->decompress(enc, enc_len, dict, out) == SECTOR_SIZE ? 0 : -1;
}
