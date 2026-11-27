/* cb-server CLI: argv parsing and the per-session report line; the sender's actual work lives in server_session.c (SPEC.md; MIGRATION_FORMAT2.md). */
#include "server_session.h"
#include "cli.h"
#include "host.h"
#include "lz4.h"
#include "frame.h"
#include "format2_geom.h"
#include "util.h"
#include "platform_posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static const char *opt_image = NULL;   /* the release to serve */
static int opt_port    = 7700;
static int opt_abort    = 0;   /* test-only: ABORT instead of COMMIT, see --abort-after-plan */
static const char *opt_hold = NULL;   /* test-only: see --hold-session-end */

static void cli_log(void *ctx, const char *line)
{
    (void)ctx;
    puts(line);
}

/* One row per update request. No old-style "of full image" wire-cost comparison (format2 has no per-step COPY/WRITE/ACK messages, SPEC.md 10); server_session_result_t carries step counts and raw wire bytes instead. */
static void report(const char *label, const server_session_result_t *r)
{
    static int header;

    if (!header) {
        printf("\n%-10s %6s %6s %6s %6s %10s %10s\n",
               "case", "erase", "copy", "write", "of", "tx B", "rx B");
        printf("%-10s %6s %6s %6s %6s %10s %10s\n",
               "----------", "------", "------", "------", "------",
               "----------", "----------");
        header = 1;
    }
    printf("%-10s %6d %6d %6d %6d %10llu %10llu\n",
           label, r->erase, r->copy, r->write, FORMAT2_CHUNKS,
           (unsigned long long)r->tx_bytes, (unsigned long long)r->rx_bytes);
    fflush(stdout);
}

static void session(plat_io_t *io)
{
    server_session_result_t res;
    cb_err_t err;
    int rc = server_update_session(io, opt_image, opt_abort, cli_log, NULL, &res, &err);
    /* The server's own fault ends this session, not the server: the
     * release may be fixed before the next device connects. */
    if (rc < 0) cli_fail("%s", err.msg);
    /* Test-only hook (tests/case_server_log_settled.sh): hold the session's
     * end, after the device has moved on, until the test opens the FIFO for
     * writing (opening it for reading blocks until then). */
    if (opt_hold) {
        FILE *gate = fopen(opt_hold, "r");
        if (gate) fclose(gate);
    }
    if (rc > 0) {
        char label[32];
        snprintf(label, sizeof label, "v%u", res.ver);
        report(label, &res);
    }
}

int main(int argc, char **argv)
{
    if (lz4_register() < 0) { cli_fail("cannot register the LZ4 compressors"); return 1; }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc)        opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--image") && i + 1 < argc)  opt_image = argv[++i];
        else if (!strcmp(argv[i], "--abort-after-plan"))       opt_abort = 1;
        else if (!strcmp(argv[i], "--hold-session-end") && i + 1 < argc)
            opt_hold = argv[++i];
        /* --cache DIR: accepted and ignored; format2 has no manifest-elision cache, but scripts/lab.sh still passes it on every server_start(), so it stays a recognized no-op. */
        else if (!strcmp(argv[i], "--cache") && i + 1 < argc)  i++;
        else { cli_fail("unknown option %s", argv[i]); return 2; }
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

    if (!opt_image) {
        cli_fail("no image to serve\n"
                 "  usage: cb-server --image FILE [--port N]");
        return 2;
    }

    plat_io_t *srv = plat_io_tcp_listen(opt_port);
    if (!srv) {
        cli_fail("cannot listen on port %d: %s\n"
                 "  another server is probably still running: pkill cb-server\n"
                 "  or pick a free port on both sides: --port N",
                 opt_port, strerror(errno));
        return 1;
    }
    printf("server: listening on 127.0.0.1:%d, serving %s\n", opt_port, opt_image);
    printf("server: a full image is %d sectors, %.0f-%.0f ms of flash on the device\n",
           SECTORS_SLOT, SECTORS_SLOT * flash_sector_timing_ms(0), SECTORS_SLOT * flash_sector_timing_ms(1));
    fflush(stdout);

    for (;;) {
        plat_io_t *io = plat_io_tcp_accept(srv);
        if (!io) continue;
        /* Under the tests stdout is a file, so block-buffered, and a
         * session's last lines (its report) are written after COMMIT, when
         * the device waits for nothing more. These two flushed lines let a
         * reader of the log wait for a session to end (tests/common.sh,
         * server_idle). */
        printf("server: session start\n");
        fflush(stdout);
        session(io);
        plat_io_close(io);
        printf("server: session end\n");
        fflush(stdout);
    }
    plat_io_close(srv);
    return 0;
}
