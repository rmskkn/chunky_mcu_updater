/* cb-server: holds released images, diffs a device manifest against the
 * target, and streams only the sectors that differ. DESIGN.md sections 5-6. */
#include "frame.h"
#include "proto.h"
#include "manifest.h"
#include "plan.h"
#include "image.h"
#include "keys.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/socket.h>

static const char *opt_images = "images";
static const char *opt_cache  = "cache";
static int opt_port    = 7700;
static int opt_full    = 0;
static int opt_no_copy = 0;
static int opt_tamper  = -1;
static int opt_once    = 0;

static uint8_t dev_key[32], root_key[32];

static int newest_version(char *path_out, size_t pathlen)
{
    DIR *d = opendir(opt_images);
    struct dirent *e;
    int best = -1;
    if (!d) die("cannot open image directory %s", opt_images);
    while ((e = readdir(d))) {
        int v;
        if (sscanf(e->d_name, "v%d.bin", &v) == 1 && v > best) best = v;
    }
    closedir(d);
    if (best >= 0) snprintf(path_out, pathlen, "%s/v%d.bin", opt_images, best);
    return best;
}

static void cache_path(char *out, size_t len, const uint8_t id[16])
{
    char hex[33];
    hexdump_line(id, 16, hex);
    snprintf(out, len, "%s/%s.manifest", opt_cache, hex);
}

static void report(const link_t *l, const plan_t *p, int full_bytes)
{
    uint64_t wire = l->tx_bytes + l->rx_bytes;
    printf("server: steps skip=%d copy=%d write=%d\n", p->n_skip, p->n_copy, p->n_write);
    printf("server: wire tx=%llu rx=%llu total=%llu bytes in %llu frames\n",
           (unsigned long long)l->tx_bytes, (unsigned long long)l->rx_bytes,
           (unsigned long long)wire, (unsigned long long)(l->tx_frames + l->rx_frames));
    printf("STATS skip=%d copy=%d write=%d tx=%llu rx=%llu wire=%llu full=%d pct=%.1f\n",
           p->n_skip, p->n_copy, p->n_write,
           (unsigned long long)l->tx_bytes, (unsigned long long)l->rx_bytes,
           (unsigned long long)wire, full_bytes,
           full_bytes ? 100.0 * (double)wire / full_bytes : 0.0);
    fflush(stdout);
}

static void session(int fd)
{
    link_t l;
    uint8_t pay[FRAME_MAX_PAYLOAD];
    size_t n;
    msg_hello_t hello;
    msg_challenge_t ch;
    manifest_t dev;
    plan_t plan;
    char path[512], cpath[512];

    link_init(&l, fd);

    if (frame_expect(&l, MSG_HELLO, pay, &n) < 0) return;
    memcpy(&hello, pay, sizeof hello);
    if (hello.sector_size != PAGE_SIZE || hello.slot_count != SLOT_COUNT ||
        hello.sectors_slot != SECTORS_SLOT) {
        uint8_t r = REJ_GEOMETRY_MISMATCH;
        frame_send(&l, MSG_MANIFEST_REJECT, &r, 1);
        printf("server: geometry mismatch, session refused\n");
        return;
    }
    printf("server: HELLO active_slot=%u running_version=%u last_result=%u\n",
           hello.active_slot, hello.running_version, hello.last_result);

    rnd_bytes(ch.nonce, NONCE_LEN);
    frame_send(&l, MSG_CHALLENGE, &ch, sizeof ch);

    /* Manifest, with elision (DESIGN.md 17.1). */
    msg_manifest_auth_t auth;
    if (frame_expect(&l, MSG_MANIFEST_DIGEST, pay, &n) < 0) return;
    memcpy(&auth, pay, sizeof auth);

    uint8_t want_mac[SHA256_LEN];
    manifest_mac(dev_key, hello.device_id, ch.nonce, auth.manifest_sha256, want_mac);
    if (memcmp(want_mac, auth.mac, SHA256_LEN)) {
        uint8_t r = REJ_BAD_SIGNATURE;
        frame_send(&l, MSG_MANIFEST_REJECT, &r, 1);
        printf("server: manifest MAC bad, session refused\n");
        return;
    }

    cache_path(cpath, sizeof cpath, hello.device_id);
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
        printf("server: manifest elided, cache hit (%zu B of body not sent)\n",
               MANIFEST_BODY_LEN);
    } else {
        frame_send(&l, MSG_MANIFEST_NEEDED, NULL, 0);
        uint8_t *body = malloc(MANIFEST_BODY_LEN);
        size_t got = 0;
        for (;;) {
            int t = frame_recv(&l, pay, &n);
            if (t < 0) { free(body); return; }
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
            printf("server: manifest rejected (%s)\n", why ? why : "digest or MAC");
            free(body);
            return;
        }
        write_file(cpath, body, MANIFEST_BODY_LEN);
        free(body);
        printf("server: manifest received and verified, %zu B of body\n", MANIFEST_BODY_LEN);
    }

    /* Target image. */
    int ver = newest_version(path, sizeof path);
    if (ver < 0) die("no images in %s", opt_images);
    if ((uint32_t)ver == hello.running_version) {
        frame_send(&l, MSG_NO_UPDATE, NULL, 0);
        printf("server: device already runs version %d, nothing to do\n", ver);
        return;
    }

    size_t binlen;
    uint8_t *bin = read_file(path, &binlen);
    if (!bin) die("cannot read %s", path);
    uint8_t *target = malloc(SLOT_SIZE);
    if (image_build(bin, binlen, (uint32_t)ver, root_key, target) < 0)
        die("%s is %zu B, over the %d B slot payload", path, binlen, IMG_MAX_PAYLOAD);
    free(bin);

    int dst_slot = hello.active_slot ? 0 : 1;
    plan_build(&dev, target, dst_slot, !opt_no_copy, opt_full, opt_tamper, &plan);

    const image_desc_t *d = image_desc(target);
    msg_plan_begin_t pb;
    memset(&pb, 0, sizeof pb);
    pb.version    = (uint32_t)ver;
    pb.dst_slot   = (uint8_t)dst_slot;
    pb.step_count = (uint16_t)plan.count;
    memcpy(pb.image_sha256, d->sha256, SHA256_LEN);
    memcpy(pb.image_mac, d->mac, SHA256_LEN);
    frame_send(&l, MSG_PLAN_BEGIN, &pb, sizeof pb);
    printf("server: plan v%d into slot %d, %d steps (%d skipped)\n",
           ver, dst_slot, plan.count, plan.n_skip);

    for (int k = 0; k < plan.count; k++) {
        step_t *s = &plan.step[k];
        if (s->op == OP_COPY) {
            msg_step_copy_t c;
            c.dst = s->dst; c.src_slot = s->src_slot; c.src_index = s->src_index;
            memcpy(c.md5, s->md5, MD5_LEN);
            frame_send(&l, MSG_STEP_COPY, &c, sizeof c);
        } else {
            uint8_t sect[PAGE_SIZE];
            memcpy(sect, target + (size_t)s->dst * PAGE_SIZE, PAGE_SIZE);
            uint8_t md[MD5_LEN];
            memcpy(md, s->md5, MD5_LEN);
            if (opt_tamper == s->dst) {
                /* Alter the bytes AND the advertised hash, so the per-sector
                 * check passes and only the image-level gate can catch it. */
                sect[7] ^= 0xFF;
                md5(sect, PAGE_SIZE, md);
                printf("server: tampering with sector %u (consistent md5)\n", s->dst);
            }
            for (size_t off = 0; off < PAGE_SIZE; off += WRITE_CHUNK) {
                uint8_t buf[5 + WRITE_CHUNK];
                uint16_t len = WRITE_CHUNK;
                uint16_t o16 = (uint16_t)off;
                buf[0] = s->dst;
                memcpy(buf + 1, &o16, 2);
                memcpy(buf + 3, &len, 2);
                memcpy(buf + 5, sect + off, len);
                frame_send(&l, MSG_STEP_WRITE, buf, 5 + len);
            }
            msg_step_write_end_t we;
            we.dst = s->dst;
            memcpy(we.md5, md, MD5_LEN);
            frame_send(&l, MSG_STEP_WRITE_END, &we, sizeof we);
        }
        int t = frame_recv(&l, pay, &n);
        if (t < 0) { printf("server: device went away after %d steps\n", k); goto out; }
        if (t == MSG_STEP_NAK) printf("server: NAK on sector %u reason %u\n", pay[0], pay[1]);
    }

    frame_send(&l, MSG_PLAN_END, NULL, 0);
    if (frame_expect(&l, MSG_PLAN_READY, pay, &n) < 0) goto out;
    printf("server: device staged the plan, %u steps left for the bootloader\n",
           (unsigned)((msg_plan_ready_t *)pay)->pending);
    frame_send(&l, MSG_COMMIT, NULL, 0);

out:
    report(&l, &plan, (int)(SLOT_SIZE + (SLOT_SIZE / WRITE_CHUNK) * FRAME_OVERHEAD));
    free(target);
}

int main(int argc, char **argv)
{
    poc_dev_key(dev_key);
    poc_root_key(root_key);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)        opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--images") && i + 1 < argc) opt_images = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)  opt_cache = argv[++i];
        else if (!strcmp(argv[i], "--tamper") && i + 1 < argc) opt_tamper = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--full"))                   opt_full = 1;
        else if (!strcmp(argv[i], "--no-copy"))                opt_no_copy = 1;
        else if (!strcmp(argv[i], "--once"))                   opt_once = 1;
        else die("unknown option %s", argv[i]);
    }

    int srv = tcp_listen(opt_port);
    if (srv < 0) die("cannot listen on port %d", opt_port);
    printf("server: listening on 127.0.0.1:%d, images from %s\n", opt_port, opt_images);
    fflush(stdout);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        session(fd);
        close(fd);
        if (opt_once) break;
    }
    close(srv);
    return 0;
}
