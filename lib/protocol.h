#ifndef CB_PROTOCOL_H
#define CB_PROTOCOL_H
#include <stdint.h>
#include "sha256.h"

/* The wire protocol, DESIGN.md section 6, and nothing else: frame layout,
 * message types, payload records, reason and result codes. No flash model,
 * no geometry, no platform -- a peer implementing the protocol needs this
 * header and SHA-256, not the rest of lib/. Records that depend on the
 * part's geometry (the manifest body, the image slot) live with that
 * geometry in manifest.h and image.h. */

/* Frame: | 0xA5 0x5A | len:u16 | type:u8 | seq:u8 | payload | crc32:u32 |
 * 2 + 2 + 1 + 1 + 4 = 10 bytes of overhead per frame. */
#define FRAME_MAGIC0      0xA5
#define FRAME_MAGIC1      0x5A
#define FRAME_HDR_LEN     6
#define FRAME_OVERHEAD    10
#define FRAME_MAX_PAYLOAD 288

/* Message types. */
enum {
    MSG_HELLO = 1, MSG_CHALLENGE, MSG_MANIFEST, MSG_MANIFEST_END,
    MSG_MANIFEST_DIGEST, MSG_MANIFEST_NEEDED, MSG_MANIFEST_REJECT,
    MSG_PLAN_BEGIN, MSG_STEP_COPY, MSG_STEP_WRITE, MSG_STEP_WRITE_END,
    MSG_STEP_ACK, MSG_STEP_NAK, MSG_PLAN_END, MSG_PLAN_READY,
    MSG_COMMIT, MSG_STATUS, MSG_ABORT, MSG_NO_UPDATE,
    MSG_PLAN_TABLE, MSG_PLAN_TABLE_END
};

/* MANIFEST_REJECT reasons */
enum { REJ_BAD_SIGNATURE = 1, REJ_BAD_NONCE, REJ_UNKNOWN_DEVICE,
       REJ_GEOMETRY_MISMATCH, REJ_MALFORMED };

/* STEP_NAK reasons */
enum { NAK_HASH_MISMATCH = 1, NAK_FLASH_FAIL, NAK_BAD_INDEX, NAK_DECOMPRESS };

/* last_result codes reported back in HELLO */
enum { RES_NONE = 0, RES_OK, RES_VERIFY_FAILED, RES_FLASH_FAIL, RES_TRIAL_FAILED };

/* Sector encodings (STEP_WRITE_END.compress), DESIGN.md 17.2.
 * 3..COMPRESS_MAX_ID are free for compressors registered by the integrator. */
enum { COMPRESS_RAW = 0, COMPRESS_LZ4 = 1, COMPRESS_LZ4_DICT = 2 };
#define COMPRESS_MAX_ID 15

#define NONCE_LEN 16

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

typedef struct {
    uint8_t  device_id[16];
    uint16_t sector_size;
    uint16_t sectors;          /* sectors per slot, not the manifest total */
    uint8_t  last_result;
    uint8_t  sector_hash_len;  /* SECTOR_HASH_LEN the device was built with */
    slot_id_t active_slot;    /* which slot is currently running (DESIGN.md section 6) */
    uint8_t  pad;
    uint32_t running_version;
    /* image_sha256 of the active slot, or zeros if it holds no image. The hash
     * excludes the descriptor (DESIGN.md 10.1), so it identifies the firmware
     * content independently of the version stamped on it. */
    uint8_t  running_sha256[SHA256_LEN];
} msg_hello_t;

typedef struct { uint8_t nonce[NONCE_LEN]; } msg_challenge_t;

/* MANIFEST_DIGEST and MANIFEST_END carry the same two fields.
 *
 * manifest_digest is the plain SHA-256 of the canonical manifest body, and
 * deliberately keyless: it identifies content, not sender. It changes only
 * when flash does, which is what lets the server cache the body and elide
 * it (DESIGN.md 17.1), and anyone can check a body against it. Do not turn
 * it into an HMAC: without a nonce that adds nothing (it would be replayable
 * as is), and with one it changes every session and kills the elision.
 *
 * mac authenticates it: HMAC-SHA256 under the device key over ManifestAuth
 * (domain, device_id, the server's nonce, manifest_digest), which gives
 * origin and freshness. Production replaces it with an ECDSA P-256
 * signature over the same structure (DESIGN.md 10.2), which needs a field
 * of its own size; the digest stays as it is. */
typedef struct {
    uint8_t manifest_digest[SHA256_LEN];
    uint8_t mac[SHA256_LEN];
} msg_manifest_auth_t;

/* The target descriptor's signed fields and its mac (DESIGN.md section 10.1),
 * so the device verifies the release signature before the first step. The
 * rest of the descriptor (revoked, reserved) is fixed by the format, so the
 * device can rebuild it whole and compare it with what lands in flash. */
typedef struct {
    uint32_t version;
    uint16_t step_count;
    slot_id_t dest_slot;   /* always the device's reported inactive slot; the
                            * device cross-checks this against its own
                            * computed pending_slot and never trusts it alone
                            * (DESIGN.md section 6: "target version, destination
                            * slot, step count..."). */
    uint8_t  pad;
    uint32_t image_len;    /* the target's real size, DESIGN.md section 10.1 */
    uint32_t flags;        /* bit0 allow_downgrade */
    uint8_t  image_sha256[SHA256_LEN];
    uint8_t  table_root[SHA256_LEN];   /* SHA-256 of the PLAN_TABLE body */
    uint8_t  image_mac[SHA256_LEN];
} msg_plan_begin_t;

/* PLAN_TABLE fragments the target's per-sector hashes, image_len /
 * SECTOR_SIZE of them, dense and in sector order (like MANIFEST: off:u16,
 * len:u16, bytes; fragments arrive in order). PLAN_TABLE_END has no payload:
 * the device checks SHA-256(table) against PLAN_BEGIN's signed table_root.
 * Steps then name a sector and nothing else; the expected hash is the
 * table's. */
#define TABLE_HDR   4

/* A copy from flash the device already has: the bytes dst needs already sit
 * at (src_slot, src_index), so nothing goes over the wire. src_slot may be
 * the active slot (always safe, untouched for the whole update) or the
 * destination slot itself (only safe when src_index is a sector this same
 * plan does not also rewrite -- DESIGN.md section 5 rule 2, section 8's
 * apply invariant). */
typedef struct {
    sector_idx_t dst;
    slot_id_t    src_slot;
    uint8_t      pad;
    sector_idx_t src_index;
} msg_step_copy_t;

/* STEP_WRITE fragments the encoded sector (offset/len index the encoded
 * stream); STEP_WRITE_END closes it with the compressor id and the hash. */
#define WRITE_CHUNK 256
/* Serialised by hand as dst:u16, offset:u16, len:u16, then the bytes. */
#define WRITE_HDR   6

/* The decoded sector must hash to the table's entry for dst. */
typedef struct {
    sector_idx_t dst;
    uint8_t      compress;     /* COMPRESS_* */
    uint8_t      pad;
    uint16_t     enc_len;
} msg_step_write_end_t;
typedef struct { sector_idx_t dst; uint8_t code; } msg_step_ack_t;
typedef struct { uint16_t pending; } msg_plan_ready_t;

typedef struct {
    uint8_t  plan_state;
    uint8_t  pad[3];
    uint32_t version;
    uint8_t  result;
} msg_status_t;

/* ManifestAuth, DESIGN.md section 4. The manifest body itself is nonce-free
 * so that manifest_digest is stable across sessions and the server can cache
 * it (section 17.1); freshness lives here instead. */
typedef struct {
    uint8_t domain[8];       /* "CBMAN\0\0\0" */
    uint8_t device_id[16];
    uint8_t nonce[NONCE_LEN];
    uint8_t manifest_digest[SHA256_LEN];
} manifest_auth_t;

#endif
