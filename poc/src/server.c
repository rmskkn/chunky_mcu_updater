/* cb-server CLI: argv parsing and the per-session report table. The sender's
 * actual work - diffing a device manifest against the target release and
 * streaming only the sectors that differ - lives in update_server.c;
 * DESIGN.md sections 5-6. */
#include "update_server.h"
#include "frame.h"
#include "plan.h"
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

static void cli_log(void *ctx, const char *line)
{
    (void)ctx;
    puts(line);
}

/* Wire cost of a raw sector and of a copy step, from frame and message sizes:
 * the raw full-image baseline is exact for any run. */
#define SECTOR_WIRE  ((SECTOR_SIZE / WRITE_CHUNK) \
                        * (FRAME_OVERHEAD + WRITE_HDR + WRITE_CHUNK) \
                      + (FRAME_OVERHEAD + (int)sizeof(msg_step_write_end_t)) \
                      + (FRAME_OVERHEAD + (int)sizeof(msg_step_ack_t)))
#define COPY_WIRE    ((FRAME_OVERHEAD + (int)sizeof(msg_step_copy_t)) \
                      + (FRAME_OVERHEAD + 2))

/* "1-7,9,20-40": sector indices, sorted and run-length folded, so a plan that
 * touches most of the slot stays one readable line. */
static void sector_list(const plan_t *p, int op, char *out, size_t cap)
{
    static int idx[SECTORS_SLOT];
    int n = 0;
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
static void report(const char *label, const server_session_result_t *r)
{
    const plan_t *p = &r->plan;
    static int header;
    double wire = (double)(r->tx_bytes + r->rx_bytes);
    /* Everything that is not a step is session overhead: manifest, plan
     * header, closing messages. A full-image transfer pays it too. */
    double fixed = wire - (double)r->write_wire
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
    printf("  encoded %llu B\n", (unsigned long long)r->enc_bytes);

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
    server_session_result_t res;
    if (server_update_session(fd, opt_image, opt_cache, dev_key, root_key,
                              cli_log, NULL, &res)) {
        char label[32];
        snprintf(label, sizeof label, "v%u", res.ver);
        report(label, &res);
    }
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
           SECTORS_SLOT, SECTORS_SLOT * f103_sector_ms(0), SECTORS_SLOT * f103_sector_ms(1));
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
