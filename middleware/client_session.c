/* The device side of the CLI: carries lib/device.c's session over a framed
 * link. The core never sees the link, only messages. */
#include "client_session.h"
#include "frame.h"

static int link_send(void *ctx, uint8_t type, const void *payload, size_t len)
{
    return frame_send(ctx, type, payload, len);
}

int client_update_session(flash_t *f, plat_io_t *io, const uint8_t dev_key[32],
                          const uint8_t root_key[32], log_fn log, void *log_ctx, device_stats_t *stats,
                          cb_err_t *err)
{
    device_session_t s;
    link_t l;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;

    link_init(&l, io);
    int rc = device_session_start(&s, f, dev_key, root_key, link_send, &l, log, log_ctx, stats);
    while (rc == DEV_PENDING) {
        int t = frame_recv(&l, pay, &n);
        if (t == FRAME_BAD)
            /* Not the server's message as sent: end the session, never
             * act on it. */
            return err_set(err, "bad frame (magic, length, CRC or seq); session ended");
        rc = device_session_feed(&s, t, pay, t < 0 ? 0 : n);
    }
    if (rc == DEV_ERROR) return err_set(err, "%s", s.error);

    log_fmt(log, log_ctx,
           "device: wire tx=%llu rx=%llu bytes (framing overhead tx=%llu rx=%llu B over %llu/%llu frames)",
           (unsigned long long)l.tx_bytes, (unsigned long long)l.rx_bytes,
           (unsigned long long)l.tx_frames * FRAME_OVERHEAD,
           (unsigned long long)l.rx_frames * FRAME_OVERHEAD,
           (unsigned long long)l.tx_frames, (unsigned long long)l.rx_frames);
    return rc == DEV_STAGED ? 0 : 1;
}
