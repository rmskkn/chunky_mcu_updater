#ifndef CB_COMPRESS_H
#define CB_COMPRESS_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "protocol.h"   /* COMPRESS_* wire ids */

/* A compression library, as the update path sees it. Dictionary-using
 * compressors get the destination sector's current flash content, copied
 * into a sector buffer by the caller. */
typedef struct {
    uint8_t     id;          /* wire id; unique */
    const char *name;
    int         uses_dict;   /* needs the destination's old content */

    /* Server. Encoded length in (0, SECTOR_SIZE), or 0 if it cannot beat raw.
     * dict is non-NULL only if uses_dict. */
    size_t (*compress)(const uint8_t in[SECTOR_SIZE], const uint8_t *dict,
                       uint8_t out[SECTOR_SIZE]);

    /* Device. Decoded length, or -1. The input is not hash-checked yet, so this
     * must be bounds-checked on enc and out (SECTOR_SIZE bytes). dict is
     * non-NULL if uses_dict. A device needs only this half. */
    int (*decompress)(const uint8_t *enc, size_t enc_len, const uint8_t *dict,
                      uint8_t out[SECTOR_SIZE]);
} compressor_t;

/* Adds a compressor; none is built in (RAW needs none). 0, or -1 if the id
 * is taken or out of range. Call before the first session, on both sides:
 * a device can decode only the ids it registered. */
int compress_register(const compressor_t *c);

/* Server. Smallest of RAW and every registered compressor (dictionary ones
 * only if dict is non-NULL); RAW unless one is strictly smaller. Returns the
 * encoded length and sets *id. */
size_t compress_sector(const uint8_t in[SECTOR_SIZE], const uint8_t *dict,
                       uint8_t out[SECTOR_SIZE], uint8_t *id);

/* Device. dict: current flash content of the destination. 0 iff exactly
 * SECTOR_SIZE bytes were produced; -1 for an unknown id, a length out of
 * range or malformed input. */
int decompress_sector(uint8_t id, const uint8_t *enc, size_t enc_len,
                      const uint8_t *dict, uint8_t out[SECTOR_SIZE]);

#endif
