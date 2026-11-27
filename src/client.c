/* cb-client CLI: argv parsing and the device-time report. The device's
 * actual work - manifest generation, the update session, the bootloader's
 * apply/verify step - lives in lib/device.c, carried over the link by
 * client_session.c; SPEC.md, MIGRATION_FORMAT2.md. */
#include "bootloader.h"
#include "client_session.h"
#include "device.h"
#include "cli.h"
#include "flash_file.h"
#include "format2.h"
#include "format2_geom.h"
#include "format2_image.h"
#include "format2_manifest.h"
#include "host.h"
#include "lz4.h"
#include "sha256.h"
#include "util.h"
#include "platform_posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* format2_manifest_build()'s own stated bound (lib/format2_manifest.h). */
#define MANIFEST_BUF_CAP (13u + 2u * (size_t)FORMAT2_CHUNKS * 9u)

static const char *opt_flash = "flash.bin";
static const char *opt_host  = "127.0.0.1";
static int opt_port = 7700;
static const char *opt_initial = NULL;
static int opt_status = 0;

static device_stats_t g_stats;

static void cli_log(void *ctx, const char *line)
{
    (void)ctx;
    puts(line);
}

/* Device-time model. The host runs the same code thousands of times faster,
 * so the interesting number is what the work would cost on the part: hashing
 * at the cycles/byte in flash.h, plus RM0008 flash erase and program times. */
static double model_ms(const flash_t *f, int worst, const device_stats_t *s)
{
    double cyc_per_ms = CB_FLASH_MHZ * 1000.0;
    return (double)s->sha_bytes * CB_FLASH_SHA256_CYC_B / cyc_per_ms +
           flash_timing_ms(f->n_erase, f->prog_bytes, worst) + s->ecdsa_ms;
}

static void time_report(const char *phase, double host_ms, const flash_t *f,
                        const device_stats_t *s)
{
    printf("time: %-10s host %8.2f ms   F103 %8.1f - %.1f ms"
           "  (flash %.1f - %.1f ms: %ld erase, %zu B prog)\n",
           phase, host_ms, model_ms(f, 0, s), model_ms(f, 1, s),
           flash_timing_ms(f->n_erase, f->prog_bytes, 0),
           flash_timing_ms(f->n_erase, f->prog_bytes, 1),
           f->n_erase, f->prog_bytes);
    if (!machine_output()) { fflush(stdout); return; }
    printf("TIME phase=%s host_ms=%.3f model_min_ms=%.1f model_max_ms=%.1f "
           "flash_min_ms=%.1f flash_max_ms=%.1f erases=%ld "
           "prog_bytes=%zu sha_bytes=%zu ecdsa_ms=%.0f\n",
           phase, host_ms, model_ms(f, 0, s), model_ms(f, 1, s),
           flash_timing_ms(f->n_erase, f->prog_bytes, 0),
           flash_timing_ms(f->n_erase, f->prog_bytes, 1),
           f->n_erase, f->prog_bytes,
           s->sha_bytes, s->ecdsa_ms);
    fflush(stdout);
}

static const char *state_name(int st)
{
    static const char *n[] = { "NONE", "STAGING", "APPLYING", "TRIAL", "COMMITTED", "BAD" };
    return (st >= 0 && st <= 5) ? n[st] : "?";
}

/* ---------------------------------------------------------------- session */

/* 0 = a plan was staged, 1 = nothing to do, -1 = failed (reported). */
static int cmd_session(void)
{
    flash_t f;
    cb_err_t err;

    if (device_open_or_create(&f, opt_flash, opt_initial, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);

    plat_io_t *io = plat_io_tcp_connect(opt_host, opt_port);
    if (!io) {
        int e = errno;
        flash_file_close(&f);
        return cli_fail("cannot connect to %s:%d: %s\n"
                        "  no server is listening there; start one in another terminal:\n"
                        "    scripts/lab.sh server",
                        opt_host, opt_port, strerror(e));
    }

    /* No key params: scheme 2 (unsigned) needs none. */
    double t_start = now_ms();
    int rc = client_update_session(&f, io, cli_log, NULL, &g_stats, &err);
    double session_ms = now_ms() - t_start;
    if (rc < 0) {
        plat_io_close(io);
        flash_file_close(&f);
        return cli_fail("%s", err.msg);
    }

    printf("time: manifest   host %8.2f ms   F103 model %8.1f ms  (%d sectors hashed)\n",
           g_stats.t_manifest, (double)MANIFEST_ENTRIES * SECTOR_SIZE * CB_FLASH_SHA256_CYC_B / (CB_FLASH_MHZ * 1000.0),
           MANIFEST_ENTRIES);
    printf("time: sign       host %8.2f ms   F103 model %8.1f ms  (ECDSA P-256, budgeted)\n",
           g_stats.t_auth, ECDSA_SIGN_MS);
    printf("time: stage      host %8.2f ms   F103 %8.1f - %.1f ms  (receive, verify, program)\n",
           g_stats.t_stage, flash_timing_ms(f.n_erase, f.prog_bytes, 0),
           flash_timing_ms(f.n_erase, f.prog_bytes, 1));
    printf("time: persist    host %8.2f ms\n", g_stats.t_persist);
    time_report("session", session_ms, &f, &g_stats);

    plat_io_close(io);
    flash_file_close(&f);
    return rc;
}

/* ------------------------------------------------------------- bootloader */

static int cmd_boot(void)
{
    flash_t f;
    cb_err_t err;

    if (device_open_or_create(&f, opt_flash, opt_initial, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);

    double t_start = now_ms();
    int rc = bootloader_boot(&f, cli_log, NULL, &g_stats);
    double boot_ms = now_ms() - t_start;
    if (rc < 0) {
        flash_file_close(&f);
        return cli_fail("boot: %s", f.err.msg);
    }

    if (g_stats.t_apply > 0)
        printf("time: apply      host %8.2f ms   F103 %8.1f - %.1f ms  (copies)\n",
               g_stats.t_apply, flash_timing_ms(f.n_erase, f.prog_bytes, 0),
               flash_timing_ms(f.n_erase, f.prog_bytes, 1));
    if (g_stats.t_verify > 0)
        /* No ECDSA_VERIFY_MS term: format2 scheme 2 is unsigned, no asymmetric verify cost to model. */
        printf("time: verify     host %8.2f ms   F103 %8.1f ms  "
               "(image_sha256)\n", g_stats.t_verify,
               (double)g_stats.verify_bytes * CB_FLASH_SHA256_CYC_B / (CB_FLASH_MHZ * 1000.0));
    time_report("boot", boot_ms, &f, &g_stats);
    flash_file_close(&f);
    return 0;
}

/* --------------------------------------------------------------- confirm */

/* The application's "I'm healthy, I reached the server" call (DESIGN.md
 * section 7). A benign no-op, not a failure, when there is no trial in
 * progress -- e.g. the default end-to-end flow calling this right after a
 * boot that found nothing to apply. */
static int cmd_confirm(void)
{
    flash_t f;
    cb_err_t err;
    meta_t m;

    if (device_open_or_create(&f, opt_flash, opt_initial, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);
    meta_load(&f, &m);
    if (m.plan_state != PLAN_TRIAL) {
        flash_file_close(&f);
        return 0;
    }
    int rc = device_confirm(&f, cli_log, NULL);
    if (rc < 0) {
        flash_file_close(&f);
        return cli_fail("confirm: %s", f.err.msg);
    }
    flash_file_close(&f);
    return 0;
}

static int cmd_status(void)
{
    flash_t f;
    meta_t m;
    uint8_t d[32];
    char hex[80];

    cb_err_t err;
    if (device_open_or_create(&f, opt_flash, opt_initial, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);
    identity_t id;
    if (identity_get(&f, &id) < 0) {
        flash_file_close(&f);
        return cli_fail("%s", f.err.msg);
    }
    meta_load(&f, &m);

    hexdump_line(id.device_id, 8, hex);
    printf("device_id       %s...\n", hex);
    printf("meta gen %u  state %s  last_result %u  active slot %c  security floor %u",
           m.generation, state_name(m.plan_state), m.last_result, 'A' + m.active_slot,
           m.security_floor);
    if (m.plan_state == PLAN_TRIAL)
        printf("  pending slot %c (%d/%d trial attempts left)",
               'A' + m.pending_slot, m.attempts_left, MAX_TRIES);
    printf("\n");

    /* Floor 0: a report, not a gate; the point is "what version is here and does it verify," not "would it pass staging today." */
    for (int slot = SLOT_A; slot <= SLOT_B; slot++) {
        uint8_t desc_raw[CHUNKY_SIGNED_DESCRIPTOR_LEN];
        chunky_descriptor_body_view_t body;
        format2_err_t e = format2_image_verify_flash(&f, slot, 0u, desc_raw, &body);
        if (e == FORMAT2_OK)
            printf("slot %c          image     version %u  verified\n",
                   'A' + slot, chunky_descriptor_body_version(body));
        else
            printf("slot %c          no image  %s\n", 'A' + slot, format2_err_str(e));
    }

    {
        uint8_t mbuf[MANIFEST_BUF_CAP];
        size_t mlen = format2_manifest_build(&f, m.active_slot, OTHER_SLOT(m.active_slot),
                                             mbuf, sizeof mbuf);
        sha256(mbuf, mlen, d);
        hexdump_line(d, 8, hex);
        printf("manifest digest %s...  %d chunks, body %zu B\n",
               hex, (int)(2 * FORMAT2_CHUNKS), mlen);
    }
    flash_file_close(&f);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: cb-client --flash FILE [options]\n"
        "  default: runs one whole update attempt end to end -- send the\n"
        "  manifest, take the sectors the server says differ, apply them,\n"
        "  trial-boot, confirm (simulating a healthy application), report.\n"
        "\n"
        "  --flash FILE       emulated flash device (required)\n"
        "  --initial FILE     firmware to create the device with, first run only\n"
        "  --host H           server address, default 127.0.0.1\n"
        "  --port N           server port, default 7700\n"
        "  --status           print the device state and exit\n"
        "  --session-only     only run the session (manifest, plan, stage) --\n"
        "                     no boot, no confirm\n"
        "  --boot-only        only run the bootloader step (apply/verify, or\n"
        "                     one more trial attempt, or revert) -- no session\n"
        "  --confirm-only     only run the application confirm step\n"
        "  --no-confirm       with the default flow, skip the auto-confirm\n"
        "                     after boot -- leaves a successful trial pending\n"
        "  (the last four simulate repeated reset cycles, or inspecting the\n"
        "  device mid-cycle, for testing the trial/confirm/revert state\n"
        "  machine: see docs/ab_migration.md)\n");
    return 2;
}

int main(int argc, char **argv)
{
    int opt_boot_only = 0, opt_confirm_only = 0, opt_no_confirm = 0, opt_session_only = 0;
    g_stats.clock_ms = now_ms;
    if (lz4_register() < 0) { cli_fail("cannot register the LZ4 decoders"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc)           opt_flash = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)       opt_host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)       opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--initial") && i + 1 < argc)    opt_initial = argv[++i];
        else if (!strcmp(argv[i], "--status"))                     opt_status = 1;
        else if (!strcmp(argv[i], "--boot-only"))                  opt_boot_only = 1;
        else if (!strcmp(argv[i], "--confirm-only"))                opt_confirm_only = 1;
        else if (!strcmp(argv[i], "--no-confirm"))                  opt_no_confirm = 1;
        else if (!strcmp(argv[i], "--session-only"))                opt_session_only = 1;
        else return usage();
    }

    if (opt_status) return cmd_status() < 0 ? 1 : 0;
    if (opt_confirm_only) return cmd_confirm() < 0 ? 1 : (cmd_status() < 0 ? 1 : 0);
    if (opt_boot_only) return cmd_boot() < 0 ? 1 : (cmd_status() < 0 ? 1 : 0);
    if (opt_session_only) return cmd_session() < 0 ? 1 : 0;

    /* Default: one whole update attempt -- session, bootloader apply and
     * trial boot, then (unless suppressed) the application's own confirm,
     * simulating the common case of a healthy image that reaches the
     * server right away. --session-only/--boot-only/--confirm-only above
     * are what a test uses to drive the trial/revert state machine, or
     * inspect the device mid-cycle, through repeated simulated resets. */
    if (cmd_session() < 0 || cmd_boot() < 0) return 1;
    if (!opt_no_confirm && cmd_confirm() < 0) return 1;
    if (cmd_status() < 0) return 1;
    return 0;
}
