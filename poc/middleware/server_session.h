#ifndef CB_SERVER_SESSION_H
#define CB_SERVER_SESSION_H
#include "plan.h"
#include "util.h"
#include "platform_io.h"
#include <stdint.h>

typedef struct {
    uint32_t ver;
    plan_t   plan;
    uint64_t tx_bytes, rx_bytes;
    int      n_raw, n_lz4, n_lz4_dict;   /* written sectors per compressor */
    uint64_t enc_bytes;                  /* encoded sector bytes */
    uint64_t write_wire;                 /* tx bytes of the write steps, framing included */
} server_session_result_t;

/* Handles one device connection end to end: HELLO, the manifest (with the
 * elision cache in cache_dir), diffing it against image_path, and streaming
 * the resulting plan. Returns 1 and fills *out if a plan was built and
 * streamed - the caller should report it - or 0 if the session ended before
 * that (geometry mismatch, bad auth, a rejected manifest, a protocol fault
 * on the device's side, or nothing to update). -1 with the reason in *err
 * is the server's own fault (the release cannot be read or does not fit),
 * which the next session will hit too. */
int server_update_session(plat_io_t *io, const char *image_path, const char *cache_dir,
                          const uint8_t dev_key[32], const uint8_t root_key[32],
                          log_fn log, void *log_ctx, server_session_result_t *out,
                          cb_err_t *err);

#endif
