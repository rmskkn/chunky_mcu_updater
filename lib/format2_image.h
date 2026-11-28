#ifndef CB_FORMAT2_IMAGE_H
#define CB_FORMAT2_IMAGE_H
#include <stdint.h>
#include <stddef.h>
#include "flash.h"
#include "format2.h"
#include "chunky_format.h"

/* format 2's image layout (SPEC.md 4): whole slot is payload, 0xFF padding, then the 128 B descriptor in the slot's *last* 128 bytes (unlike the old layout's middle-of-slot DESC_OFF). */

/* SHA-256 over [slot_base, slot_base+FORMAT2_IMAGE_LEN) (SPEC.md 4.1's image hash), read in FLASH_READ_CHUNK pieces, never the whole slot at once. */
void format2_image_hash_flash(flash_t *f, int slot, uint8_t out[32]);

/* Checks the slot's trailing descriptor, then its image_sha256 against format2_image_hash_flash() (SPEC.md 4.3). desc_raw is caller-owned: on success *out_body points into it, so it must outlive out_body's use; a local buffer here caused a stack-use-after-return caught by ASan. */
format2_err_t format2_image_verify_flash(flash_t *f, int slot, uint32_t floor,
                                         uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN],
                                         chunky_descriptor_body_view_t *out_body);

/* Lays bin at offset 0, 0xFF-pads the rest, then writes and seals a CKY2 descriptor (scheme 2 unsigned, SPEC.md 4.2) at the end. 0 on success, -1 if binlen is too big for the slot; never truncates. */
int format2_image_build(const uint8_t *bin, size_t binlen, uint32_t version,
                        uint32_t security_counter, uint8_t out[SLOT_SIZE], size_t *image_len);

#endif
