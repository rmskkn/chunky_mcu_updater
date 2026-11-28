/* The sender's functional core (SPEC.md; MIGRATION_FORMAT2.md): HELLO, the device's manifest, diffing the target release against the device, then streaming the plan; cb-server.c is the thin CLI shell around this. */
#include "server_session.h"
#include "frame.h"
#include "protocol.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_image.h"
#include "format2_plan.h"
#include "host.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

/* format2_manifest_build()'s stated bound (lib/format2_manifest.h), mirrored here since the server receives the device's buffer rather than building its own. */
#define MANIFEST_BUF_CAP (13u + 2u * (size_t)FORMAT2_CHUNKS * 9u)

/* The plan this server streams: 145 B header plus worst-case body (every chunk WRITE-fragmented); same bound as lib/device.h's DEVICE_PLAN_BUF_CAP. */
#define PLAN_BUF_CAP (CHUNKY_PLAN_HEADER_LEN + (size_t)FORMAT2_BLOCKS * 16u + (size_t)SLOT_SIZE)

typedef struct { int erase, copy, write; } step_counts_t;

static int count_step(void *ctx, const format2_step_t *step, uint32_t index)
{
    (void)index;
    step_counts_t *c = ctx;
    if (step->op == CHUNKY_STEP_ERASE)      c->erase++;
    else if (step->op == CHUNKY_STEP_COPY)  c->copy++;
    else if (step->op == CHUNKY_STEP_WRITE) c->write++;
    return 0;
}

/* Receives MSG2_MANIFEST fragments (off:u16, len:u16, PLAN2_FRAG_HDR) into buf until MSG2_MANIFEST_END. Returns the total received, or 0 on a protocol fault (the caller ends the session). */
static size_t recv_manifest(link_t *l, uint8_t *buf, size_t cap, log_fn log, void *log_ctx)
{
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n, got = 0;

    for (;;) {
        int t = frame_recv(l, pay, &n);
        if (t < 0) return 0;
        if (t == MSG2_MANIFEST_END) return got;
        if (t != MSG2_MANIFEST) {
            log_fmt(log, log_ctx, "server: protocol: expected MANIFEST, got %d; session ended", t);
            return 0;
        }
        uint16_t off, len;
        if (n < PLAN2_FRAG_HDR) {
            log_fmt(log, log_ctx, "server: short MANIFEST fragment (%zu B)", n);
            return 0;
        }
        memcpy(&off, pay, 2);
        memcpy(&len, pay + 2, 2);
        if (off != got || (size_t)off + len > cap || len > n - PLAN2_FRAG_HDR) {
            log_fmt(log, log_ctx, "server: bad MANIFEST fragment (off=%u len=%u, have %zu B)",
                   off, len, got);
            return 0;
        }
        memcpy(buf + off, pay + PLAN2_FRAG_HDR, len);
        got += len;
    }
}

/* Streams buf as MSG2_PLAN fragments (off:u16, len:u16, PLAN2_FRAG_HDR), closed by MSG2_PLAN_END; same shape recv_manifest() above decodes and lib/device.c's own MANIFEST send uses. */
static void send_fragmented(link_t *l, uint8_t type, uint8_t end_type,
                            const uint8_t *buf, size_t len)
{
    size_t off = 0;
    const size_t max_body = FRAME_MAX_PAYLOAD - PLAN2_FRAG_HDR;

    while (off < len) {
        uint8_t frag[FRAME_MAX_PAYLOAD];
        uint16_t o16 = (uint16_t)off;
        uint16_t flen = (uint16_t)(len - off < max_body ? len - off : max_body);
        memcpy(frag, &o16, 2);
        memcpy(frag + 2, &flen, 2);
        memcpy(frag + PLAN2_FRAG_HDR, buf + off, flen);
        frame_send(l, type, frag, PLAN2_FRAG_HDR + flen);
        off += flen;
    }
    frame_send(l, end_type, NULL, 0);
}

int server_update_session(plat_io_t *io, const char *image_path,
                           int abort_before_commit,
                           log_fn log, void *log_ctx,
                           server_session_result_t *out, cb_err_t *err)
{
    link_t l;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    msg2_hello_t hello;
    uint8_t manifest_buf[MANIFEST_BUF_CAP];
    uint8_t *target = NULL, *plan = NULL, *active_scratch = NULL;
    size_t manifest_got, image_len, plan_len;
    step_counts_t counts = { 0 };

    link_init(&l, io);
    memset(out, 0, sizeof *out);

    if (frame_expect(&l, MSG2_HELLO, pay, &n) < 0) return 0;
    memcpy(&hello, pay, sizeof hello);
    if (hello.geometry_id != format2_geometry_id() || hello.product_id != CB_PRODUCT_ID ||
        (hello.active_slot != SLOT_A && hello.active_slot != SLOT_B)) {
        /* format2's message space has no reject reply (unlike the old MSG_MANIFEST_REJECT); dropping the link is the signal the session never got anywhere. */
        log_fmt(log, log_ctx, "server: geometry/product mismatch, session refused");
        return 0;
    }
    log_fmt(log, log_ctx, "server: HELLO running_version=%u active_slot=%c",
           hello.running_version, 'A' + hello.active_slot);

    /* The device's manifest (SPEC.md 7.2: unsigned, untrusted by design) is received and sanity-checked to keep the wire protocol in sync, but NOT used to plan COPY-vs-WRITE below; see server_session.h. */
    manifest_got = recv_manifest(&l, manifest_buf, sizeof manifest_buf, log, log_ctx);
    if (manifest_got == 0) return 0;
    {
        uint16_t chunks = 0;
        format2_err_t me = format2_manifest_begin(manifest_buf, manifest_got,
                                                   format2_geometry_id(), &chunks);
        log_fmt(log, log_ctx, "server: manifest received, %zu B (%s)", manifest_got,
               me == FORMAT2_OK ? "well-formed" : format2_err_str(me));
    }

    /* Target image: re-read from image_path every session, so a new release can be dropped in without a restart. version = device's + 1; security_counter rides along with it (scheme 2 has no separate release-signing step). */
    size_t binlen;
    uint8_t *bin = read_file(image_path, &binlen);
    if (!bin) return err_set(err, "cannot read %s", image_path);
    uint32_t ver = hello.running_version + 1;
    target = malloc(SLOT_SIZE);
    if (!target) { free(bin); return err_set(err, "out of memory for a %d B slot image", SLOT_SIZE); }
    if (format2_image_build(bin, binlen, ver, ver, target, &image_len) < 0) {
        free(bin);
        free(target);
        return err_set(err, "%s is %zu B, over the %u B slot payload",
                       image_path, binlen, (unsigned)FORMAT2_IMAGE_LEN);
    }
    free(bin);

    /* Compares HELLO's running_sha256, not the manifest (per-chunk, untrusted); the whole-image hash is the one value format2_image_verify_flash() itself trusts (SPEC.md 4.3). */
    uint8_t target_sha[32];
    sha256(target, image_len, target_sha);
    if (!memcmp(hello.running_sha256, target_sha, 32)) {
        frame_send(&l, MSG2_NO_UPDATE, NULL, 0);
        log_fmt(log, log_ctx, "server: device already runs this image, nothing to do");
        free(target);
        return 0;
    }

    /* The server has no live flash_t for the device, so no real ACTIVE bytes to compare (the manifest carries hashes, not bytes); it plans against an all-blank scratch "active" instead, which turns every non-blank target chunk into WRITE (safe, just less efficient, per SPEC.md). A blank target chunk is still skipped, so this can't manufacture a bogus COPY. */
    active_scratch = malloc(SLOT_SIZE);
    if (!active_scratch) {
        free(target);
        return err_set(err, "out of memory for a %d B scratch slot", SLOT_SIZE);
    }
    memset(active_scratch, 0xFF, SLOT_SIZE);

    plan = malloc(PLAN_BUF_CAP);
    if (!plan) {
        free(target);
        free(active_scratch);
        return err_set(err, "out of memory for a %zu B plan buffer", PLAN_BUF_CAP);
    }
    plan_len = format2_plan_build(active_scratch, target, plan, PLAN_BUF_CAP);
    free(active_scratch);
    active_scratch = NULL;
    if (plan_len == 0) {
        free(target);
        free(plan);
        return err_set(err, "format2: plan build failed (buffer too small, or the target "
                       "has no valid descriptor)");
    }

    /* Step counts for the report: re-walks the plan with the device's own decoder rather than threading counters through format2_plan_build(). read_active may be NULL: the all-blank scratch guarantees no COPY step was emitted. */
    {
        chunky_plan_header_view_t h;
        uint8_t scratch[FORMAT2_CHUNK_LEN];
        if (chunky_plan_header_view(plan, plan_len, &h) == 0) {
            uint32_t step_count = chunky_plan_header_step_count(h);
            format2_plan_walk(plan, plan_len, step_count, (uint32_t)FORMAT2_BLOCKS,
                              (uint32_t)FORMAT2_CHUNKS, (uint16_t)FORMAT2_CHUNK_LEN,
                              NULL, NULL, scratch, count_step, &counts);
        }
    }
    log_fmt(log, log_ctx, "server: plan v%u, %d erase %d copy %d write, %zu B",
           ver, counts.erase, counts.copy, counts.write, plan_len);

    send_fragmented(&l, MSG2_PLAN, MSG2_PLAN_END, plan, plan_len);

    int t = frame_recv(&l, pay, &n);
    if (t < 0) {
        log_fmt(log, log_ctx, "server: device went away waiting for the plan's ack");
        goto out_report;
    }
    if (t == MSG2_STEP_NAK) {
        msg2_step_ack_t nak;
        memcpy(&nak, pay, sizeof nak);
        log_fmt(log, log_ctx, "server: device NAKed the plan, code %u", nak.code);
        goto out_report;
    }
    if (t != MSG2_STEP_ACK) {
        log_fmt(log, log_ctx, "server: protocol: expected STEP_ACK/NAK, got %d; session ended", t);
        goto out_report;
    }
    log_fmt(log, log_ctx, "server: device staged the plan");

    if (abort_before_commit) {
        /* Test-only hook: simulate the link/server giving up right after the device acked the plan; since it's never armed, bootloader_boot() finds it staged but unarmed and unrolls it. */
        frame_send(&l, MSG2_ABORT, NULL, 0);
        log_fmt(log, log_ctx, "server: test hook, sending ABORT instead of COMMIT");
        goto out_report;
    }
    frame_send(&l, MSG2_COMMIT, NULL, 0);

out_report:
    out->ver   = ver;
    out->erase = counts.erase;
    out->copy  = counts.copy;
    out->write = counts.write;
    out->tx_bytes = l.tx_bytes;
    out->rx_bytes = l.rx_bytes;
    free(target);
    free(plan);
    return 1;
}
