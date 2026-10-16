/* The sender's functional core: diffs a device manifest against the target
 * release and streams only the sectors that differ (DESIGN.md sections 5-6).
 * No argv parsing, no printf/report table: status goes through the log
 * callback, the finished plan/wire counts through server_session_result_t.
 * cb-server.c is the thin CLI shell around this. */
#include "update_server.h"
#include "frame.h"
#include "proto.h"
#include "manifest.h"
#include "image.h"
#include "compress.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static void cache_path(char *out, size_t len, const uint8_t id[16], const char *cache_dir)
{
    char hex[33];
    hexdump_line(id, 16, hex);
    snprintf(out, len, "%s/%s.manifest", cache_dir, hex);
}

/* Sector store: every sector of every image served, by content hash. The
 * device's current content of a sector is what its manifest hash names, so a
 * hit gives the compressor the exact bytes the device will hold as dictionary.
 * Files are trusted by content, never by name. */
static void store_path(char *out, size_t len, const char *cache_dir,
                       const uint8_t h[SECTOR_HASH_LEN])
{
    char hex[2 * SECTOR_HASH_LEN + 1];
    hexdump_line(h, SECTOR_HASH_LEN, hex);
    snprintf(out, len, "%s/sectors/%s", cache_dir, hex);
}

static void store_put(const char *cache_dir, const uint8_t *sector)
{
    uint8_t h[SECTOR_HASH_LEN];
    char path[512], tmp[560];

    sector_hash(sector, h);
    store_path(path, sizeof path, cache_dir, h);
    if (access(path, F_OK) == 0) return;
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());   /* rename: no torn files */
    if (write_file(tmp, sector, SECTOR_SIZE) < 0 || rename(tmp, path) < 0) unlink(tmp);
}

static int store_get(const char *cache_dir, const uint8_t h[SECTOR_HASH_LEN],
                     uint8_t out[SECTOR_SIZE])
{
    char path[512];
    size_t n = 0;
    uint8_t got[SECTOR_HASH_LEN];

    store_path(path, sizeof path, cache_dir, h);
    uint8_t *b = read_file(path, &n);
    if (!b) return -1;
    if (n == SECTOR_SIZE) memcpy(out, b, SECTOR_SIZE);
    free(b);
    if (n != SECTOR_SIZE) return -1;
    sector_hash(out, got);
    return memcmp(got, h, SECTOR_HASH_LEN) ? -1 : 0;
}

int server_update_session(plat_io_t *io, const char *image_path, const char *cache_dir,
                          const uint8_t dev_key[32], const uint8_t root_key[32],
                          log_fn log, void *log_ctx, server_session_result_t *out)
{
    link_t l;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    msg_hello_t hello;
    msg_challenge_t ch;
    manifest_t dev;
    char cpath[512];

    link_init(&l, io);
    memset(out, 0, sizeof *out);

    if (frame_expect(&l, MSG_HELLO, pay, &n) < 0) return 0;
    memcpy(&hello, pay, sizeof hello);
    if (hello.sector_size != SECTOR_SIZE || hello.sectors != SECTORS_SLOT) {
        uint8_t r = REJ_GEOMETRY_MISMATCH;
        frame_send(&l, MSG_MANIFEST_REJECT, &r, 1);
        log_fmt(log, log_ctx, "server: geometry mismatch, session refused");
        return 0;
    }
    log_fmt(log, log_ctx, "server: HELLO running_version=%u last_result=%u",
           hello.running_version, hello.last_result);

    rnd_bytes(ch.nonce, NONCE_LEN);
    frame_send(&l, MSG_CHALLENGE, &ch, sizeof ch);

    /* Manifest, with elision (DESIGN.md 17.1). */
    msg_manifest_auth_t auth;
    if (frame_expect(&l, MSG_MANIFEST_DIGEST, pay, &n) < 0) return 0;
    memcpy(&auth, pay, sizeof auth);

    uint8_t want_mac[SHA256_LEN];
    manifest_mac(dev_key, hello.device_id, ch.nonce, auth.manifest_sha256, want_mac);
    if (memcmp(want_mac, auth.mac, SHA256_LEN)) {
        uint8_t r = REJ_BAD_SIGNATURE;
        frame_send(&l, MSG_MANIFEST_REJECT, &r, 1);
        log_fmt(log, log_ctx, "server: manifest MAC bad, session refused");
        return 0;
    }

    cache_path(cpath, sizeof cpath, hello.device_id, cache_dir);
    int cached = 0;
    size_t clen = 0;
    uint8_t *cbuf = read_file(cpath, &clen);
    if (cbuf && clen == MANIFEST_BODY_LEN) {
        uint8_t d[SHA256_LEN];
        sha256(cbuf, clen, d);
        if (!memcmp(d, auth.manifest_sha256, SHA256_LEN)) {
            memcpy(&dev, cbuf, MANIFEST_BODY_LEN);
            cached = 1;
        }
    }
    free(cbuf);

    if (cached) {
        log_fmt(log, log_ctx, "server: manifest elided, cache hit (%zu B of body not sent)",
               MANIFEST_BODY_LEN);
    } else {
        frame_send(&l, MSG_MANIFEST_NEEDED, NULL, 0);
        uint8_t *body = malloc(MANIFEST_BODY_LEN);
        size_t got = 0;
        for (;;) {
            int t = frame_recv(&l, pay, &n);
            if (t < 0) { free(body); return 0; }
            if (t == MSG_MANIFEST_END) break;
            if (t != MSG_MANIFEST) die("protocol: expected MANIFEST, got %d", t);
            uint16_t off, len;
            memcpy(&off, pay, 2);
            memcpy(&len, pay + 2, 2);
            if ((size_t)off + len > MANIFEST_BODY_LEN) die("manifest fragment out of range");
            memcpy(body + off, pay + 4, len);
            got += len;
        }
        memcpy(&auth, pay, sizeof auth);
        uint8_t d[SHA256_LEN];
        sha256(body, MANIFEST_BODY_LEN, d);
        manifest_mac(dev_key, hello.device_id, ch.nonce, auth.manifest_sha256, want_mac);
        const char *why = NULL;
        memcpy(&dev, body, MANIFEST_BODY_LEN);

        if (got != MANIFEST_BODY_LEN || memcmp(d, auth.manifest_sha256, SHA256_LEN) ||
            memcmp(want_mac, auth.mac, SHA256_LEN) || manifest_check(&dev, &why)) {
            uint8_t r = why ? REJ_MALFORMED : REJ_BAD_SIGNATURE;
            frame_send(&l, MSG_MANIFEST_REJECT, &r, 1);
            log_fmt(log, log_ctx, "server: manifest rejected (%s)", why ? why : "digest or MAC");
            free(body);
            return 0;
        }
        write_file(cpath, body, MANIFEST_BODY_LEN);
        free(body);
        log_fmt(log, log_ctx, "server: manifest received and verified, %zu B of body", MANIFEST_BODY_LEN);
    }

    /* Target image: whatever file was named on the command line, re-read each
     * session so a new release can be dropped in without a restart. The
     * version is the device's own plus one, which keeps it monotonic without
     * the server tracking anything. */
    size_t binlen;
    uint8_t *bin = read_file(image_path, &binlen);
    if (!bin) die("cannot read %s", image_path);
    uint32_t ver = hello.running_version + 1;
    uint8_t *target = malloc(SLOT_SIZE);
    if (image_build(bin, binlen, ver, root_key, target) < 0)
        die("%s is %zu B, over the %d B slot payload", image_path, binlen, IMG_MAX_PAYLOAD);
    free(bin);

    /* The first session after factory install finds no history and falls back
     * to plain LZ4; from then on every served sector is a future dictionary. */
    snprintf(cpath, sizeof cpath, "%s/sectors", cache_dir);
    mkdir(cpath, 0755);
    for (int i = 0; i < SECTORS_SLOT; i++)
        store_put(cache_dir, target + (size_t)i * SECTOR_SIZE);

    /* The device already runs this firmware if its active slot has the same
     * content hash. image_sha256 excludes the descriptor, so the version
     * stamp does not affect the comparison. */
    if (!memcmp(hello.running_sha256, image_desc(target)->sha256, SHA256_LEN)) {
        frame_send(&l, MSG_NO_UPDATE, NULL, 0);
        log_fmt(log, log_ctx, "server: device already runs this image, nothing to do");
        free(target);
        return 0;
    }

    double t_plan = now_ms();
    plan_build(&dev, target, &out->plan);
    t_plan = now_ms() - t_plan;
    log_fmt(log, log_ctx, "server: plan built in %.2f ms", t_plan);

    const image_desc_t *d = image_desc(target);
    msg_plan_begin_t pb;
    memset(&pb, 0, sizeof pb);
    pb.version    = (uint32_t)ver;
    pb.step_count = (uint16_t)out->plan.count;
    memcpy(pb.image_sha256, d->sha256, SHA256_LEN);
    memcpy(pb.image_mac, d->mac, SHA256_LEN);
    frame_send(&l, MSG_PLAN_BEGIN, &pb, sizeof pb);
    log_fmt(log, log_ctx, "server: plan v%d, %d steps (%d skipped)",
           ver, out->plan.count, out->plan.n_skip);

    /* Stream every step without waiting for its ACK (DESIGN.md 6, revised):
     * the device flashes one sector in the background while the next one's
     * bytes are still arriving, so a lockstep wait here would just reintroduce
     * the stall the double buffer on the device side is there to hide. ACKs
     * are drained as a second pass, after PLAN_END, in send order. */
    for (int k = 0; k < out->plan.count; k++) {
        step_t *s = &out->plan.step[k];
        if (s->op == OP_COPY) {
            msg_step_copy_t c;
            c.dst = s->dst; c.src_index = s->src_index;
            memcpy(c.hash, s->hash, SECTOR_HASH_LEN);
            frame_send(&l, MSG_STEP_COPY, &c, sizeof c);
        } else {
            /* dev.h[dst] is the device's current content of dst, which is what
             * its flash holds when it decompresses: each dst is written once
             * per plan, and writes happen in-session. */
            uint8_t enc[SECTOR_SIZE], dict[SECTOR_SIZE], method;
            int have_dict = store_get(cache_dir, dev.h[s->dst], dict) == 0;
            size_t enc_len = compress_sector(target + (size_t)s->dst * SECTOR_SIZE,
                                             have_dict ? dict : NULL, enc, &method);
            out->n_raw      += method == COMPRESS_RAW;
            out->n_lz4      += method == COMPRESS_LZ4;
            out->n_lz4_dict += method == COMPRESS_LZ4_DICT;
            out->enc_bytes  += enc_len;

            for (size_t off = 0; off < enc_len; off += WRITE_CHUNK) {
                uint8_t buf[WRITE_HDR + WRITE_CHUNK];
                uint16_t len = (uint16_t)(enc_len - off < WRITE_CHUNK ? enc_len - off : WRITE_CHUNK);
                uint16_t o16 = (uint16_t)off;
                memcpy(buf + 0, &s->dst, 2);
                memcpy(buf + 2, &o16, 2);
                memcpy(buf + 4, &len, 2);
                memcpy(buf + WRITE_HDR, enc + off, len);
                frame_send(&l, MSG_STEP_WRITE, buf, WRITE_HDR + len);
                out->write_wire += FRAME_OVERHEAD + WRITE_HDR + len;
            }
            msg_step_write_end_t we = { .dst = s->dst, .compress = method, .enc_len = (uint16_t)enc_len };
            memcpy(we.hash, s->hash, SECTOR_HASH_LEN);
            frame_send(&l, MSG_STEP_WRITE_END, &we, sizeof we);
            out->write_wire += FRAME_OVERHEAD + sizeof we + FRAME_OVERHEAD + sizeof(msg_step_ack_t);
        }
    }

    log_fmt(log, log_ctx, "server: compress raw=%d lz4=%d lz4_dict=%d",
           out->n_raw, out->n_lz4, out->n_lz4_dict);

    frame_send(&l, MSG_PLAN_END, NULL, 0);

    for (int k = 0; k < out->plan.count; k++) {
        int t = frame_recv(&l, pay, &n);
        if (t < 0) {
            log_fmt(log, log_ctx, "server: device went away after %d of %d acks", k, out->plan.count);
            goto out_report;
        }
        if (t == MSG_STEP_NAK) {
            msg_step_ack_t nak;
            memcpy(&nak, pay, sizeof nak);
            log_fmt(log, log_ctx, "server: NAK on sector %u reason %u", nak.dst, nak.code);
        } else if (t != MSG_STEP_ACK) {
            die("protocol: expected STEP_ACK/NAK, got %d", t);
        }
    }

    if (frame_expect(&l, MSG_PLAN_READY, pay, &n) < 0) goto out_report;
    log_fmt(log, log_ctx, "server: device staged the plan, %u steps left for the bootloader",
           (unsigned)((msg_plan_ready_t *)pay)->pending);
    frame_send(&l, MSG_COMMIT, NULL, 0);

out_report:
    out->ver = ver;
    out->tx_bytes = l.tx_bytes;
    out->rx_bytes = l.rx_bytes;
    free(target);
    return 1;
}
