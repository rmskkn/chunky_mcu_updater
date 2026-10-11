#ifndef CB_PROTO_H
#define CB_PROTO_H
#include <stdint.h>
#include "flash.h"

/* Message types, DESIGN.md section 6. */
enum {
    MSG_HELLO = 1, MSG_CHALLENGE, MSG_MANIFEST, MSG_MANIFEST_END,
    MSG_MANIFEST_DIGEST, MSG_MANIFEST_NEEDED, MSG_MANIFEST_REJECT,
    MSG_PLAN_BEGIN, MSG_STEP_COPY, MSG_STEP_WRITE, MSG_STEP_WRITE_END,
    MSG_STEP_ACK, MSG_STEP_NAK, MSG_PLAN_END, MSG_PLAN_READY,
    MSG_COMMIT, MSG_STATUS, MSG_ABORT, MSG_NO_UPDATE
};

/* MANIFEST_REJECT reasons */
enum { REJ_BAD_SIGNATURE = 1, REJ_BAD_NONCE, REJ_UNKNOWN_DEVICE,
       REJ_GEOMETRY_MISMATCH, REJ_MALFORMED };

/* STEP_NAK reasons */
enum { NAK_MD5_MISMATCH = 1, NAK_FLASH_FAIL, NAK_BAD_INDEX };

#define NONCE_LEN 16

typedef struct {
    uint8_t  device_id[16];
    uint16_t sector_size;
    uint16_t sectors;
    uint8_t  last_result;
    uint8_t  pad[3];
    uint32_t running_version;
    /* image_sha256 of the active slot, or zeros if it holds no image. The hash
     * excludes the descriptor (DESIGN.md 10.1), so it identifies the firmware
     * content independently of the version stamped on it. */
    uint8_t  running_sha256[SHA256_LEN];
} msg_hello_t;

typedef struct { uint8_t nonce[NONCE_LEN]; } msg_challenge_t;

/* MANIFEST_DIGEST and MANIFEST_END carry the same two fields: the hash of the
 * canonical manifest body and the device MAC over ManifestAuth. */
typedef struct {
    uint8_t manifest_sha256[SHA256_LEN];
    uint8_t mac[SHA256_LEN];
} msg_manifest_auth_t;

typedef struct {
    uint32_t version;
    uint16_t step_count;
    uint8_t  pad[2];
    uint8_t  image_sha256[SHA256_LEN];
    uint8_t  image_mac[SHA256_LEN];
} msg_plan_begin_t;

/* A copy inside the one partition: the bytes this sector needs already sit at
 * src_index, so nothing goes over the wire. */
typedef struct {
    sector_idx_t dst;
    sector_idx_t src_index;
    uint8_t      hash[SECTOR_HASH_LEN];
} msg_step_copy_t;

/* STEP_WRITE fragments a sector; STEP_WRITE_END closes it with the hash. */
#define WRITE_CHUNK 256
/* Serialised by hand as dst:u16, offset:u16, len:u16, then the bytes. */
#define WRITE_HDR   6

typedef struct { sector_idx_t dst; uint8_t hash[SECTOR_HASH_LEN]; } msg_step_write_end_t;
typedef struct { sector_idx_t dst; uint8_t code; } msg_step_ack_t;
typedef struct { uint16_t pending; } msg_plan_ready_t;

typedef struct {
    uint8_t  plan_state;
    uint8_t  pad[3];
    uint32_t version;
    uint8_t  result;
} msg_status_t;

/* ManifestAuth, DESIGN.md section 4. The manifest body itself is nonce-free
 * so that manifest_sha256 is stable across sessions and the server can cache
 * it (section 17.1); freshness lives here instead. */
typedef struct {
    uint8_t domain[8];       /* "CBMAN\0\0\0" */
    uint8_t device_id[16];
    uint8_t nonce[NONCE_LEN];
    uint8_t manifest_sha256[SHA256_LEN];
} manifest_auth_t;

#endif
