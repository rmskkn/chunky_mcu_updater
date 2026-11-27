#ifndef CB_FORMAT2_GEOM_H
#define CB_FORMAT2_GEOM_H
#include <stdint.h>
#include "flash.h"

/* Geometry constants format 2 (SPEC.md 2) needs, mapped from this device's geometry.h per MIGRATION_FORMAT2.md's reconciliation table; chunk/erase both reconcile to SECTOR_SIZE, giving chunks_per_block == 1, so a format2 ERASE step always erases exactly one sector. */
#define FORMAT2_CHUNK_LEN   SECTOR_SIZE
#define FORMAT2_ERASE_LEN   SECTOR_SIZE
#define FORMAT2_BLOCKS      SECTORS_SLOT
#define FORMAT2_CHUNKS      SECTORS_SLOT
#define FORMAT2_IMAGE_LEN   (SLOT_SIZE - 128)

/* FNV-1a 32 over 13 little-endian u32 words (SPEC.md 2); a per-build constant, agreed on only because device and server compile against the same geometry.h, never negotiated per-session. */
uint32_t format2_geometry_id(void);

#endif
