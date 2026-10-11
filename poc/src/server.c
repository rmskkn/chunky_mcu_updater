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
#include <errno.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/stat.h>
#include <sys/socket.h>

static const char *opt_image = NULL;   /* the release to serve */
static const char *opt_cache  = "cache";
static int opt_port    = 7700;

static uint8_t dev_key[32], root_key[32];

static void cache_path(char *out, size_t len, const uint8_t id[16])
{
    char hex[33];
    hexdump_line(id, 16, hex);
    snprintf(out, len, "%s/%s.manifest", opt_cache, hex);
}

/* What one written sector and one copy step cost on the wire, from the frame
 * and message sizes rather than from a measurement, so the full-image baseline
 * is exact for any run. */
#define SECTOR_WIRE  ((PAGE_SIZE / WRITE_CHUNK) * (FRAME_OVERHEAD + 5 + WRITE_CHUNK) \
                      + (FRAME_OVERHEAD + 1 + MD5_LEN)   /* STEP_WRITE_END */ \
                      + (FRAME_OVERHEAD + 2))            /* STEP_ACK */
#define COPY_WIRE    ((FRAME_OVERHEAD + (int)sizeof(msg_step_copy_t)) \
                      + (FRAME_OVERHEAD + 2))

/* "1-7,9,20-40": sector indices, sorted and run-length folded, so a plan that
 * touches most of the slot stays one readable line. */
static void sector_list(const plan_t *p, int op, char *out, size_t cap)
{
    int idx[SECTORS_SLOT], n = 0;
    size_t used = 0;

    for (int k = 0; k < p->count; k++)
        if (p->step[k].op == op) idx[n++] = p->step[k].dst;
    for (int i = 1; i < n; i++)                 /* insertion sort, n <= 52 */
        for (int j = i; j > 0 && idx[j - 1] > idx[j]; j--) {
            int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t;
        }
    if (!n) { snprintf(out, cap, "-"); return; }

    for (int i = 0; i < n; ) {
        int j = i;
        while (j + 1 < n && idx[j + 1] == idx[j] + 1) j++;
        int w = (j > i)
            ? snprintf(out + used, cap - used, "%s%d-%d", used ? "," : "", idx[i], idx[j])
            : snprintf(out + used, cap - used, "%s%d", used ? "," : "", idx[i]);
        if (w < 0 || (size_t)w >= cap - used) { snprintf(out + used, cap - used, ",..."); return; }
        used += (size_t)w;
        i = j + 1;
    }
}

/* One row per update request, so repeated requests build the table up. */
static void report(const char *label, const link_t *l, const plan_t *p)
{
    static int header;
    double wire = (double)(l->tx_bytes + l->rx_bytes);
    /* Everything that is not a step is session overhead: manifest, plan
     * header, closing messages. A full-image transfer pays it too. */
    double fixed = wire - (double)p->n_write * SECTOR_WIRE
                        - (double)p->n_copy * COPY_WIRE;
    double full = fixed + (double)SECTORS_SLOT * SECTOR_WIRE;

    /* Copies program the destination as well, so both count as sectors
     * written, and flash time is what the device spends on them. */
    int sectors = p->n_copy + p->n_write;
    char flash[32];
    snprintf(flash, sizeof flash, "%.0f-%.0f",
             sectors * f103_sector_ms(0), sectors * f103_sector_ms(1));

    char wl[320], cl[320];
    sector_list(p, OP_WRITE, wl, sizeof wl);
    sector_list(p, OP_COPY, cl, sizeof cl);
    printf("sectors: %d of %d differ; %d sent, %d copied on the device, %d already right\n",
           p->n_copy + p->n_write, SECTORS_SLOT, p->n_write, p->n_copy, p->n_skip);
    printf("  sent    %s\n", wl);
    printf("  copied  %s\n", cl);

    if (!header) {
        printf("\n%-26s %4s %4s %5s %8s %7s %9s %7s %13s\n",
               "case", "skip", "copy", "write", "wire", "of full",
               "saved B", "saved", "flash ms");
        printf("%-26s %4s %4s %5s %8s %7s %9s %7s %13s\n",
               "--------------------------", "----", "----", "-----",
               "--------", "-------", "---------", "-------", "-------------");
        header = 1;
    }
    printf("%-26s %4d %4d %5d %8.0f %6.1f%% %9.0f %6.1f%% %13s\n",
           label, p->n_skip, p->n_copy, p->n_write, wire,
           100.0 * wire / full, full - wire, 100.0 * (full - wire) / full, flash);
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
    char cpath[512];

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

    /* Target image: whatever file was named on the command line, re-read each
     * session so a new release can be dropped in without a restart. The
     * version is the device's own plus one, which keeps it monotonic without
     * the server tracking anything. */
    size_t binlen;
    uint8_t *bin = read_file(opt_image, &binlen);
    if (!bin) die("cannot read %s", opt_image);
    uint32_t ver = hello.running_version + 1;
    uint8_t *target = malloc(SLOT_SIZE);
    if (image_build(bin, binlen, ver, root_key, target) < 0)
        die("%s is %zu B, over the %d B slot payload", opt_image, binlen, IMG_MAX_PAYLOAD);
    free(bin);

    /* The device already runs this firmware if its active slot has the same
     * content hash. image_sha256 excludes the descriptor, so the version
     * stamp does not affect the comparison. */
    if (!memcmp(hello.running_sha256, image_desc(target)->sha256, SHA256_LEN)) {
        frame_send(&l, MSG_NO_UPDATE, NULL, 0);
        printf("server: device already runs this image, nothing to do\n");
        free(target);
        return;
    }

    int dst_slot = hello.active_slot ? 0 : 1;
    double t_plan = now_ms();
    plan_build(&dev, target, dst_slot, &plan);
    t_plan = now_ms() - t_plan;
    printf("server: plan built in %.2f ms\n", t_plan);

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
    {
        char label[32];
        snprintf(label, sizeof label, "v%u into slot %d", ver, dst_slot);
        report(label, &l, &plan);
    }
    free(target);
}

int main(int argc, char **argv)
{
    poc_dev_key(dev_key);
    poc_root_key(root_key);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)        opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc)  opt_image = argv[++i];
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)  opt_cache = argv[++i];
        else die("unknown option %s", argv[i]);
    }

    /* A device that dies mid-plan closes the socket under us; without this the
     * server would take SIGPIPE and exit, which is useless for a long-running
     * test session. */
    signal(SIGPIPE, SIG_IGN);
#ifdef __linux__
    /* Die with whoever started us. Ctrl-C already reaches the server because
     * lab.sh execs it, but a closed terminal or a killed wrapper script would
     * otherwise leave it holding the port. */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    mkdir(opt_cache, 0755);

    if (!opt_image)
        die("no image to serve\n"
            "  usage: cb-server --image FILE [--port N] [--cache DIR]");

    int srv = tcp_listen(opt_port);
    if (srv < 0)
        die("cannot listen on port %d: %s\n"
            "  another server is probably still running: pkill cb-server\n"
            "  or pick a free port on both sides: --port N",
            opt_port, strerror(errno));
    printf("server: listening on 127.0.0.1:%d, serving %s\n", opt_port, opt_image);
    printf("server: a full image is %d sectors, %.0f-%.0f ms of flash on the device\n",
           SECTORS_SLOT, SECTORS_SLOT * f103_sector_ms(0),
           SECTORS_SLOT * f103_sector_ms(1));
    fflush(stdout);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) continue;
        session(fd);
        close(fd);
    }
    close(srv);
    return 0;
}
