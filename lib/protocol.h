#ifndef CB_PROTOCOL_H
#define CB_PROTOCOL_H
#include <stdint.h>
#include "sha256.h"

/* The wire protocol: frame layout, shared slot/sector/result types, and format 2's message set (SPEC.md; MIGRATION_FORMAT2.md); no flash model, geometry or platform, so a peer needs only this, lib/format2.h and SHA-256. The legacy MSG_* set was deleted with the format2 migration (no reader left). */

/* Frame: | 0xA5 0x5A | len:u16 | type:u8 | seq:u8 | payload | crc32:u32 |
 * 2 + 2 + 1 + 1 + 4 = 10 bytes of overhead per frame. */
#define FRAME_MAGIC0      0xA5
#define FRAME_MAGIC1      0x5A
#define FRAME_HDR_LEN     6
#define FRAME_OVERHEAD    10
#define FRAME_MAX_PAYLOAD 288

/* last_result codes reported back in HELLO */
enum { RES_NONE = 0, RES_OK, RES_VERIFY_FAILED, RES_FLASH_FAIL, RES_TRIAL_FAILED };

/* Sector encodings (STEP_WRITE_END.compress), DESIGN.md 17.2.
 * 3..COMPRESS_MAX_ID are free for compressors registered by the integrator. */
enum { COMPRESS_RAW = 0, COMPRESS_LZ4 = 1, COMPRESS_LZ4_DICT = 2 };
#define COMPRESS_MAX_ID 15

/* Slot ids, DESIGN.md section 2: exactly two application slots, A and B.
 * Wire-level and geometry-independent -- unlike sector_idx_t, this never
 * grows with the partition, so it lives here rather than in flash.h.
 * SLOT_NONE is an internal (never-on-the-wire) sentinel for "no slot": a
 * fresh device's pending_slot, or one a plan just unrolled/reverted. */
enum { SLOT_A = 0, SLOT_B = 1, SLOT_NONE = 0xFF };
typedef uint8_t slot_id_t;

/* Sector indices are partition-relative. A byte covers 256 sectors, which is
 * a 256 KB partition at a 1 KB sector and runs out on a 1 MB part, so the index
 * has its own type everywhere it appears: in the manifest, on the wire, and in
 * the plan the bootloader reads back. */
typedef uint16_t sector_idx_t;

/* Per-sector hash: SHA-256 truncated to its first SECTOR_HASH_LEN bytes
 * (NIST SP 800-107 allows truncation; collision strength is half the
 * kept bits). It answers one question -- does this sector already hold the
 * bytes the target needs -- and is not a trust boundary: image_sha256 and
 * the release signature gate what boots, so a collision, accidental or
 * planted, costs a failed update, never a corrupt device. At 8 B an
 * accidental collision among 2048 sectors is ~1e-13. The width sets the
 * manifest's size (one hash per sector), which is the floor of every
 * request that misses the server's cache.
 *
 * A build-time choice (make SECTOR_HASH_LEN=16), anywhere from 8 to 32.
 * Raise it if a per-sector hash ever becomes a trust anchor on its own, or
 * is wrapped in HMAC-SHA256 (RFC 2104 section 5 wants a tag of at least 80
 * bits, so 16 B). Both peers must be built with the same value: HELLO
 * carries it and the server refuses a mismatch. Reusing SHA-256 also
 * keeps a second hash primitive out of the bootloader image. */
#ifndef CB_SECTOR_HASH_LEN
#define CB_SECTOR_HASH_LEN 8
#endif
#define SECTOR_HASH_LEN CB_SECTOR_HASH_LEN
_Static_assert(SECTOR_HASH_LEN >= 8 && SECTOR_HASH_LEN <= SHA256_LEN,
               "SECTOR_HASH_LEN must be 8 to 32 bytes of SHA-256");

/* Format 2's own message-type space (SPEC.md; MIGRATION_FORMAT2.md). Values start at 100, a leftover from sharing this header with the now-deleted legacy MSG_* enum; kept as-is so no existing literal value shifts. */
enum {
    MSG2_HELLO = 100, MSG2_MANIFEST, MSG2_PLAN, MSG2_PLAN_END,
    MSG2_STEP_ACK, MSG2_STEP_NAK, MSG2_COMMIT, MSG2_STATUS,
    MSG2_ABORT, MSG2_NO_UPDATE,
    /* Closes a run of MSG2_MANIFEST fragments (same framing as MSG2_PLAN/_END): a real manifest routinely exceeds FRAME_MAX_PAYLOAD. Appended, not inserted, so it doesn't renumber the values above. */
    MSG2_MANIFEST_END
};

/* HELLO, format2: no challenge/nonce needed (scheme 2); sent immediately before MSG2_MANIFEST. running_version/running_sha256 report 0/all-zero if the active slot doesn't hold a verifying image. */
typedef struct {
    uint8_t   device_id[16];
    uint32_t  geometry_id;    /* format2_geometry_id(); device and server must agree */
    uint32_t  product_id;     /* CB_PRODUCT_ID */
    slot_id_t active_slot;
    uint8_t   last_result;
    uint8_t   pad[2];
    uint32_t  running_version;
    uint8_t   running_sha256[32];
} msg2_hello_t;

/* MSG2_PLAN fragments the plan stream (SPEC.md 6.1: 145 B header + body): off:u16, len:u16, then bytes, in order. MSG2_PLAN_END closes the buffer; format2_plan_walk() then runs over it whole (bulk delivery, not per-step). */
#define PLAN2_FRAG_HDR 4

/* Sent once, after the whole plan is applied, not per step (bulk delivery, MIGRATION_FORMAT2.md): code is 0 on ACK, a format2_err_t on NAK, or 0xFF for a flash fault with no format2_err_t code. */
typedef struct { uint8_t code; } msg2_step_ack_t;
#define NAK2_FLASH_FAIL 0xFFu

typedef struct {
    uint8_t  plan_state;
    uint8_t  pad[3];
    uint32_t version;
    uint8_t  result;
} msg2_status_t;

#endif
