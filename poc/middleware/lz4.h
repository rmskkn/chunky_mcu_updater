#ifndef CB_LZ4_H
#define CB_LZ4_H
#include "compress.h"

/* The host's two compressors, both LZ4 via liblz4. The dictionary one uses
 * the destination's old content: a release that shifts code by a few bytes
 * leaves each sector a displaced copy of itself. */
extern const compressor_t lz4_compressor;
extern const compressor_t lz4_dict_compressor;

/* Registers both with lib/compress.c. 0, or -1 if either id is taken. */
int lz4_register(void);

#endif
