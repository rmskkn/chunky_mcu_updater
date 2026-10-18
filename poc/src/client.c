/* cb-client CLI: argv parsing and the device-time report. The device's
 * actual work - manifest generation, the update session, the bootloader's
 * apply/verify step - lives in lib/device.c, carried over the link by
 * client_session.c; DESIGN.md sections 4, 6, 7, 8. */
#include "client_session.h"
#include "cli.h"
#include "flash_file.h"
#include "host.h"
#include "lz4.h"
#include "manifest.h"
#include "image.h"
#include "keys.h"
#include "util.h"
#include "platform_posix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
    static const char *n[] = { "NONE", "STAGING", "APPLYING", "COMMITTED", "BAD" };
    return (st >= 0 && st <= 4) ? n[st] : "?";
}

/* ---------------------------------------------------------------- session */

/* 0 = a plan was staged, 1 = nothing to do, -1 = failed (reported). */
static int cmd_session(void)
{
    flash_t f;
    cb_err_t err;
    uint8_t dev_key[32], root_key[32];
    poc_dev_key(dev_key);
    poc_root_key(root_key);

    if (device_open_or_create(&f, opt_flash, opt_initial, root_key, cli_log, NULL, &err) < 0)
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

    double t_start = now_ms();
    int rc = client_update_session(&f, io, dev_key, cli_log, NULL, &g_stats, &err);
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
    uint8_t root_key[32];
    poc_root_key(root_key);

    if (device_open_or_create(&f, opt_flash, opt_initial, root_key, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);

    double t_start = now_ms();
    int rc = device_boot(&f, root_key, cli_log, NULL, &g_stats);
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
        printf("time: verify     host %8.2f ms   F103 %8.1f ms  "
               "(image_sha256 + signature)\n", g_stats.t_verify,
               (double)(SLOT_SIZE - DESC_LEN) * CB_FLASH_SHA256_CYC_B / (CB_FLASH_MHZ * 1000.0)
               + ECDSA_VERIFY_MS);
    time_report("boot", boot_ms, &f, &g_stats);
    flash_file_close(&f);
    return 0;
}

static int cmd_status(void)
{
    flash_t f;
    meta_t m;
    manifest_t man;
    uint8_t d[SHA256_LEN], rk[32];
    char hex[80];
    const char *why;

    cb_err_t err;
    poc_root_key(rk);
    if (device_open_or_create(&f, opt_flash, opt_initial, rk, cli_log, NULL, &err) < 0)
        return cli_fail("%s", err.msg);
    identity_t id;
    if (identity_get(&f, &id) < 0) {
        flash_file_close(&f);
        return cli_fail("%s", f.err.msg);
    }
    meta_load(&f, &m);

    hexdump_line(id.device_id, 8, hex);
    printf("device_id       %s...\n", hex);
    printf("meta gen %u  state %s  last_result %u\n",
           m.generation, state_name(m.plan_state), m.last_result);

    image_desc_t desc;
    uint8_t h[SHA256_LEN];
    image_desc_read(&f, &desc);
    image_hash_flash(&f, h);
    int ok = image_verify_flash(&f, rk, h, &why);
    printf("application     %s  version %u  %s\n",
           desc.magic == IMG_MAGIC ? "image   " : "no image",
           device_app_version(&f), ok == 0 ? "verified" : why);

    manifest_build(&f, &man);
    manifest_digest(&man, d);
    hexdump_line(d, 8, hex);
    printf("manifest digest %s...  %d sectors, body %zu B\n",
           hex, SECTORS_SLOT, MANIFEST_BODY_LEN);
    flash_file_close(&f);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: cb-client --flash FILE [options]\n"
        "  runs one whole update attempt: send the manifest, take the sectors\n"
        "  the server says differ, apply them, verify, boot and confirm.\n"
        "\n"
        "  --flash FILE       emulated flash device (required)\n"
        "  --initial FILE     firmware to create the device with, first run only\n"
        "  --host H           server address, default 127.0.0.1\n"
        "  --port N           server port, default 7700\n"
        "  --status           print the device state and exit\n");
    return 2;
}

int main(int argc, char **argv)
{
    g_stats.clock_ms = now_ms;
    if (lz4_register() < 0) { cli_fail("cannot register the LZ4 decoders"); return 1; }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--flash") && i + 1 < argc)           opt_flash = argv[++i];
        else if (!strcmp(argv[i], "--host") && i + 1 < argc)       opt_host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc)       opt_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--initial") && i + 1 < argc)    opt_initial = argv[++i];
        else if (!strcmp(argv[i], "--status"))                     opt_status = 1;
        else return usage();
    }

    if (opt_status) return cmd_status() < 0 ? 1 : 0;

    /* Ask for an update by reporting what is in flash, then reboot into the
     * bootloader to apply and verify it. One partition, so the write is in
     * place and there is no trial boot to pass. */
    if (cmd_session() < 0 || cmd_boot() < 0 || cmd_status() < 0) return 1;
    return 0;
}
