#ifndef CB_FORMAT2_H
#define CB_FORMAT2_H
#include <stdint.h>
#include <stddef.h>

#include "chunky_format.h"

/* An independent reader/verifier for chunky's "format 2" wire contract:
 * SPEC.md (vendored at the repo root) is the spec in prose; chunky_format.h
 * (rust/chunky_format/c, generated and vendored unmodified from
 * github.com/romixlab/chunky_mcu_updater PR #5) holds the length-checked
 * views and field readers this module calls. Section numbers in comments
 * below are SPEC.md's. Per-chunk hashing matches the reference Rust
 * implementation's definition byte for byte
 * (github.com/romixlab/chunky_mcu_updater, main,
 * chunky/src/hash.rs's ChunkHash::of / chunky/src/apply.rs's Applier::run):
 * confirmed against the vendored golden vectors (every WRITE's carried hash
 * equals the first 8 bytes of SHA-256 of its data, computed independently
 * while writing this module).
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
 *     The Ed25519 scheme (scheme 1) is a known, well-formed value --
 *     format2.c refuses it with FORMAT2_ESCHEME_UNSUPPORTED rather than
 *     silently treating it as checked. There is no Ed25519 implementation
 *     anywhere in this tree; do not read FORMAT2_ESCHEME_UNSUPPORTED as
 *     "verified".
 *   - format2_plan_walk() checks every chunk hash SPEC.md 6.2 names: a
 *     WRITE's data against its carried hash (its bytes are already in the
 *     plan buffer), and a COPY's source against its carried hash, read from
 *     ACTIVE through the caller-supplied format2_read_active_fn. A COPY
 *     with no such callback given refuses with FORMAT2_ENOACTIVE rather
 *     than silently skipping the check -- the same "refuse rather than
 *     guess" choice as the Ed25519 case.
 *   - What format2_plan_walk does NOT do, because it never touches DFU
 *     flash: SPEC.md 6.2's "destination chunk reads blank" (NotBlank) and
 *     "read it back and check the hash" (VerifyFailed) after programming.
 *     Those belong to whoever actually calls flash_erase_block()/
 *     flash_program() from fn's callback, not to this decoder.
 *   - format2_manifest_begin/_walk do no hash checking: the manifest is
 *     unsigned and untrusted by design (SPEC.md 7.2 -- "a wrong one only
 *     wastes a transfer, because every COPY and WRITE is checked on the
 *     device"), so there is nothing to check here.
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
    FORMAT2_EORDER,             /* a step's position did not strictly increase (SPEC.md 5.4) */
    FORMAT2_ERANGE,             /* a step names a block/chunk index outside the geometry (SPEC.md 6.2.5, OutOfRange) */
    FORMAT2_EDATALEN,           /* a WRITE's byte count is not exactly the geometry's chunk size (SPEC.md 6.2.4) */
    FORMAT2_EDATA,              /* a WRITE's data does not have the hash the step names (SPEC.md 6.2, DataMismatch) */
    FORMAT2_ESOURCE,            /* a COPY's source does not have the hash the step names (SourceMismatch) */
    FORMAT2_ESOURCE_UNREADABLE, /* the read_active callback reported an unreadable/ECC-faulted source (SourceUnreadable) */
    FORMAT2_ENOACTIVE           /* a COPY step needs ACTIVE's bytes to check and no read_active callback was given */
} format2_err_t;

/* Text for a log line; never NULL, never allocates. */
const char *format2_err_str(format2_err_t e);

/* The per-chunk hash, SPEC.md section 2: the first 8 bytes of SHA-256 over
 * exactly the data bytes, no domain separation (unlike the descriptor
 * seal, SPEC.md 4.2's "chunky image descriptor v2" prefix) -- the same
 * definition the Rust reference implementation uses
 * (chunky::hash::ChunkHash::of). out must hold 8 bytes. */
#define FORMAT2_CHUNK_HASH_LEN 8u
void format2_chunk_hash(const uint8_t *data, size_t len, uint8_t out[FORMAT2_CHUNK_HASH_LEN]);

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
 * On FORMAT2_OK, *descriptor_body (if non-NULL) receives the view of the
 * 64 sealed body bytes inside buf, so a caller can read further fields
 * (version, image SHA-256) with chunky_format.h's own accessors. */
format2_err_t format2_plan_begin(const uint8_t *buf, size_t len,
                                 uint32_t geometry_id, uint32_t product_id,
                                 uint32_t floor, uint32_t image_len,
                                 chunky_descriptor_body_view_t *descriptor_body);

/* One decoded step (SPEC.md section 5.2), already checked against its
 * carried hash by the time format2_plan_walk's callback sees it: a WRITE's
 * data matched its hash, and a COPY's source (read through
 * format2_read_active_fn) matched its hash. hash is 8 bytes and NULL for
 * ERASE; data is WRITE's chunk and NULL otherwise. Pointers are slices into
 * the caller's buffer -- nothing here copies, except the scratch buffer
 * format2_plan_walk uses internally to read a COPY's source. */
typedef struct {
    uint8_t        op;    /* CHUNKY_STEP_ERASE / _COPY / _WRITE, chunky_format.h */
    uint16_t       a;     /* ERASE: block; COPY/WRITE: dst */
    uint16_t       b;     /* COPY: src; WRITE: data byte count */
    const uint8_t *hash;  /* 8 bytes, COPY/WRITE only */
    const uint8_t *data;  /* WRITE only, b bytes */
} format2_step_t;

/* Called once per step, in stream order, after its hash (if any) has
 * already been checked. A nonzero return stops the walk and becomes
 * format2_plan_walk's return value (so a caller can fold its own error
 * code through this, e.g. a negative cb_err_t-style result). */
typedef int (*format2_step_fn)(void *ctx, const format2_step_t *step, uint32_t index);

/* Supplies the chunk_len bytes of ACTIVE's chunk chunk_index, for checking
 * a COPY's source against its carried hash (SPEC.md 5.2: "COPY programs
 * chunk dst ... with the bytes of chunk src of ACTIVE, which MUST have
 * chunk hash hash"). Returns 0 having filled buf, or nonzero when the
 * chunk is unreadable (an ECC fault: a write or erase cut by power loss,
 * SPEC.md's floor/ecc notes) -- format2_plan_walk then reports
 * FORMAT2_ESOURCE_UNREADABLE rather than treating it as a pass. */
typedef int (*format2_read_active_fn)(void *ctx, uint16_t chunk_index, uint8_t *buf, uint16_t chunk_len);

/* Walks exactly step_count steps of the plan body starting right after the
 * 145-byte header (SPEC.md section 5.3: each step is framed from its own
 * head before any WRITE data is touched), in the order of SPEC.md 6.2: a
 * WRITE's byte count must equal chunk_len (FORMAT2_EDATALEN); every index
 * must be in range, dst/src < chunks and an ERASE's block < blocks
 * (FORMAT2_ERANGE); a step's position must be strictly greater than the one
 * before it (FORMAT2_EORDER, section 5.4: position(ERASE block b) =
 * 2*b*(chunks/blocks), position(COPY/WRITE chunk c) = 2*c+1); then, by op,
 * a WRITE's data (already in the buffer) is checked against its carried
 * hash, and a COPY's source is read via read_active and checked against
 * its carried hash -- read_active may be NULL only when step_count's steps
 * are known in advance to contain no COPY (any COPY then refuses with
 * FORMAT2_ENOACTIVE rather than skipping the check). scratch must be at
 * least chunk_len bytes; format2_plan_walk uses it only to hold a COPY's
 * source while hashing it.
 *
 * Returns 0 once step_count steps are walked and the body is exactly
 * consumed (no trailing bytes); a negative -(format2_err_t) on a framing,
 * range, order or hash problem; or whatever nonzero value fn returned. */
int format2_plan_walk(const uint8_t *buf, size_t len, uint32_t step_count,
                      uint32_t blocks, uint32_t chunks, uint16_t chunk_len,
                      format2_read_active_fn read_active, void *active_ctx,
                      uint8_t *scratch,
                      format2_step_fn fn, void *ctx);

/* One decoded manifest chunk entry: HASH carries 8 bytes, BLANK and
 * UNREADABLE carry none (hash is NULL). Tag values are chunky_format.h's
 * CHUNKY_CHUNK_ENTRY_HASH/_BLANK/_UNREADABLE. The manifest is unsigned and
 * untrusted by design (SPEC.md 7.2), so neither format2_manifest_begin nor
 * format2_manifest_walk check a hash against anything -- there is nothing
 * to check it against; a wrong entry only costs a wasted transfer, caught
 * by format2_plan_walk's checks instead. */
typedef struct {
    uint8_t        tag;
    const uint8_t *hash; /* 8 bytes, HASH only */
} format2_entry_t;

typedef int (*format2_entry_fn)(void *ctx, const format2_entry_t *entry, uint32_t index);

/* Checks the 13-byte manifest header: magic CKM2, format 2, a known scheme
 * (FORMAT2_ESCHEME otherwise, SPEC.md section 3), geometry id.
 * *chunks_out (if non-NULL) receives the header's chunk count; a manifest
 * carries 2 * chunks entries (ACTIVE's, then DFU's, SPEC.md section 7). */
format2_err_t format2_manifest_begin(const uint8_t *buf, size_t len,
                                     uint32_t geometry_id, uint16_t *chunks_out);

/* Walks exactly entry_count entries right after the 13-byte manifest
 * header. Same return convention as format2_plan_walk. */
int format2_manifest_walk(const uint8_t *buf, size_t len, uint32_t entry_count,
                          format2_entry_fn fn, void *ctx);

#endif
