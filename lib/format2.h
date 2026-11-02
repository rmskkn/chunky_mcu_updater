#ifndef CB_FORMAT2_H
#define CB_FORMAT2_H
#include <stdint.h>
#include <stddef.h>

/* An independent reader/verifier for chunky's "format 2" wire contract:
 * SPEC.md (vendored at the repo root) is the spec in prose; chunky_format.h
 * (rust/chunky_format/c, generated and vendored unmodified from
 * github.com/romixlab/chunky_mcu_updater PR #5) holds the byte-offset
 * accessors this module calls. Section numbers in comments below are
 * SPEC.md's.
 *
 * This is new, additive code. Nothing in lib/manifest.c, lib/manifest.h,
 * lib/plan.h or lib/protocol.h is touched: the device's own wire format
 * (DESIGN.md sections 4-6) is a different, earlier layout and is not
 * byte-compatible with format 2 (no magic, no geometry id, no seal, a fixed
 * per-slot sector array instead of tagged variable-length entries). This
 * module exists to decode and verify format-2 buffers -- e.g. a plan or
 * manifest produced by a format-2-capable peer -- it does not replace
 * plan_build()/manifest_build() or feed bootloader_boot().
 *
 * Scope, read before wiring this into anything that flashes:
 *
 *   - The UNSIGNED descriptor/manifest scheme (SPEC.md 4.2 table, scheme 2)
 *     is fully checked here, using the SHA-256 already in lib/sha256.h.
 *   - The Ed25519 scheme (scheme 1) is a known, well-formed value -- format2.c
 *     refuses it with FORMAT2_ESCHEME_UNSUPPORTED rather than silently
 *     treating it as checked. There is no Ed25519 implementation anywhere in
 *     this tree (lib/sha256.h is the only primitive); do not read
 *     FORMAT2_ESCHEME_UNSUPPORTED as "verified", and do not call code past
 *     it on an Ed25519-scheme descriptor until a real verify is added.
 *   - Step/entry decoding (format2_plan_walk, format2_manifest_walk) is
 *     signature-independent and works for both schemes; it only reads what
 *     chunky_format.h's tagged-variant walkers (chunky_step_tail,
 *     chunky_chunk_entry_tail) already bounds-check.
 *
 * Error convention matches manifest_check() (lib/manifest.h): a function
 * returns 0 (FORMAT2_OK) or a positive format2_err_t; format2_err_str()
 * turns it into text for a log line. Nothing here allocates or exits.
 */

typedef enum {
    FORMAT2_OK = 0,
    FORMAT2_ESHORT,              /* fewer bytes than a fixed header/field needs */
    FORMAT2_EMAGIC,              /* CKP2/CKY2/CKM2 magic did not match */
    FORMAT2_EFORMAT,             /* format byte was not 2 */
    FORMAT2_ESCHEME,             /* scheme tag is neither Ed25519 (1) nor unsigned (2) */
    FORMAT2_ESCHEME_UNSUPPORTED, /* scheme is Ed25519: known, but not checkable here (see header) */
    FORMAT2_ERESERVED,          /* a reserved field was not all zero */
    FORMAT2_EGEOMETRY,          /* geometry id did not match the caller's */
    FORMAT2_ELENGTH,            /* image length / body length / step count out of bounds, or trailing bytes */
    FORMAT2_ESIGNATURE,         /* the seal did not check out (SPEC.md 4.2/4.3 stage 2) */
    FORMAT2_EPRODUCT,           /* product id did not match the caller's */
    FORMAT2_EROLLBACK,          /* security counter below the caller's floor */
    FORMAT2_ESTEP,              /* a step or manifest entry did not parse (unknown tag, truncated) */
    FORMAT2_EORDER              /* a step's position did not strictly increase (SPEC.md 5.4) */
} format2_err_t;

/* Text for a log line; never NULL, never allocates. */
const char *format2_err_str(format2_err_t e);

/* Checks a whole plan stream (the 145-byte header plus its body, SPEC.md
 * section 6.1): every refusal that needs no key first (magic, format,
 * geometry id, step count/body length bounds, the raw descriptor body's
 * stage-1 fields), then the seal over the exact 64 sealed body bytes, then
 * the fields that decide whether the image is acceptable (product id,
 * geometry id again from the sealed copy, security counter vs floor) -- all
 * read only after the seal has passed, per SPEC.md 4.3.
 *
 * image_len is the device geometry's image_len (SPEC.md section 2:
 * slot_len - 128), the value the descriptor's image length field must equal.
 *
 * On FORMAT2_OK, *descriptor_body (if non-NULL) receives a pointer to the
 * 64 sealed body bytes inside buf, so a caller can read further fields
 * (version, image SHA-256) with chunky_format.h's own accessors. */
format2_err_t format2_plan_begin(const uint8_t *buf, size_t len,
                                 uint32_t geometry_id, uint32_t product_id,
                                 uint32_t floor, uint32_t image_len,
                                 const uint8_t **descriptor_body);

/* One decoded step (SPEC.md section 5.2). hash is 8 bytes and NULL for
 * ERASE; data is WRITE's chunk and NULL otherwise. Pointers are slices into
 * the caller's buffer -- nothing here copies. */
typedef struct {
    uint8_t        op;    /* CHUNKY_STEP_ERASE / _COPY / _WRITE, chunky_format.h */
    uint16_t       a;     /* ERASE: block; COPY/WRITE: dst */
    uint16_t       b;     /* COPY: src; WRITE: data byte count */
    const uint8_t *hash;  /* 8 bytes, COPY/WRITE only */
    const uint8_t *data;  /* WRITE only, b bytes */
} format2_step_t;

/* Called once per step, in stream order. A nonzero return stops the walk
 * and becomes format2_plan_walk's return value (so a caller can fold its
 * own error code through this, e.g. a negative cb_err_t-style result). */
typedef int (*format2_step_fn)(void *ctx, const format2_step_t *step, uint32_t index);

/* Walks exactly step_count steps of the plan body starting right after the
 * 145-byte header (SPEC.md section 5.3: each step is framed from its own
 * head before any WRITE data is touched), checking section 5.4's order rule
 * as it goes: a step's position must be strictly greater than the one
 * before it, position(ERASE block b) = 2*b*chunks_per_block,
 * position(COPY/WRITE chunk c) = 2*c+1. chunks_per_block is the caller's
 * geometry (erase / chunk); pass the device's own value.
 *
 * Returns 0 once step_count steps are walked and the body is exactly
 * consumed (no trailing bytes); a negative -(format2_err_t) on a framing,
 * step-tag or ordering problem; or whatever nonzero value fn returned. */
int format2_plan_walk(const uint8_t *buf, size_t len, uint32_t step_count,
                      uint32_t chunks_per_block,
                      format2_step_fn fn, void *ctx);

/* One decoded manifest chunk entry: HASH carries 8 bytes, BLANK and
 * UNREADABLE carry none (hash is NULL). Tag values are chunky_format.h's
 * CHUNKY_CHUNK_ENTRY_HASH/_BLANK/_UNREADABLE. */
typedef struct {
    uint8_t        tag;
    const uint8_t *hash; /* 8 bytes, HASH only */
} format2_entry_t;

typedef int (*format2_entry_fn)(void *ctx, const format2_entry_t *entry, uint32_t index);

/* Checks the 13-byte manifest header: magic CKM2, format 2, geometry id.
 * The manifest is unsigned in layer 0 (SPEC.md, and PR #5's description);
 * there is no seal to check here. *chunks_out (if non-NULL) receives the
 * header's chunk count; a manifest carries 2 * chunks entries
 * (SPEC.md's chunk-present/blank bitmap convention -- see the vectors). */
format2_err_t format2_manifest_begin(const uint8_t *buf, size_t len,
                                     uint32_t geometry_id, uint16_t *chunks_out);

/* Walks exactly entry_count entries right after the 13-byte manifest
 * header. Same return convention as format2_plan_walk. */
int format2_manifest_walk(const uint8_t *buf, size_t len, uint32_t entry_count,
                          format2_entry_fn fn, void *ctx);

#endif
