#ifndef CB_FORMAT2_MANIFEST_H
#define CB_FORMAT2_MANIFEST_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"

/* SPEC.md 7: the device's unsigned, untrusted manifest: 13 B CKM2 header, then FORMAT2_CHUNKS entries for active_slot then dfu_slot (1 B BLANK if all-0xFF, else a 9 B HASH entry); UNREADABLE is never emitted, since this flash model has no ECC-fault state. Returns bytes written (at most 13+2*FORMAT2_CHUNKS*9), or 0 if out_cap is too small. */
size_t format2_manifest_build(flash_t *f, int active_slot, int dfu_slot,
                              uint8_t *out, size_t out_cap);

#endif
