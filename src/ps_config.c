/**
 * @file ps_config.c
 * @brief CLI argument parsing and configuration — Phase 1 implementation.
 *
 * Uses getopt_long() from <getopt.h> (POSIX, available in all glibc targets).
 * No third-party argument-parsing library is used; this keeps the code
 * self-contained and auditable.
 *
 * Run modes (mutually exclusive):
 *   proxSense --live         Run with real hardware (Phase 2+)
 *   proxSense --simulate F   Replay a deterministic scenario file
 *   proxSense --list-interfaces  List detected interfaces and exit
 *
 * The default run mode is --live (which will fail gracefully if no hardware
 * is present, e.g. in WSL2).
 */

#include "ps_config.h"
#include "ps_log.h"
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>

#define PROXSENSE_VERSION "0.1.0-phase4b"

/* =========================================================================
 * Defaults
 * ========================================================================= */

ps_config_t ps_config_default(void)
{
    ps_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.run_mode          = PS_MODE_LIVE;
    cfg.duration_s        = 0;  /* 0 = run indefinitely */
    cfg.report_interval_s = 5;
    cfg.verbose           = false;
    cfg.quiet             = false;
    cfg.enable_bt         = true;
    cfg.enable_wifi       = true;

    cfg.bt       = ps_bt_scanner_default_config();
    cfg.wifi     = ps_wifi_scanner_default_config();
    cfg.filter   = ps_filter_config_default();
    cfg.estimator= ps_estimator_config_default();

    /* Simulation config defaults: built-in scenario, non-realtime. */
    cfg.sim.scenario_file = NULL;
    cfg.sim.realtime      = true; /* realtime in live use; tests override */

    return cfg;
}

/* =========================================================================
 * Help text
 * ========================================================================= */

void ps_config_print_help(const char *program_name)
{
    printf(
        "ProxSense %s — Bluetooth & Wi-Fi Based Nearby Person Estimator\n"
        "\n"
        "USAGE:\n"
        "  %s --live [options]            Scan with real hardware\n"
        "  %s --simulate <scenario_file>  Replay a scenario (no hardware needed)\n"
        "  %s --list-interfaces           List BT/Wi-Fi interfaces and exit\n"
        "\n"
        "SCAN OPTIONS:\n"
        "  --duration <s>      Scan duration in seconds (default: run until Ctrl-C)\n"
        "  --interval <s>      How often to print an estimate (default: 5s)\n"
        "  --bt-hci <N>        Use hciN Bluetooth adapter (default: 0)\n"
        "  --wifi-interface <name> Select the Linux Wi-Fi interface (default: auto)\n"
        "  --wifi-iface <name> Alias retained for compatibility\n"
        "  --no-bt             Disable Bluetooth scanner\n"
        "  --no-wifi           Disable Wi-Fi scanner\n"
        "  --bt-records-per-person <n>   Estimator calibration value (placeholder by default)\n"
        "  --wifi-records-per-person <n> Estimator calibration value (placeholder by default)\n"
        "  --radio-overlap <0..1>        Overlap of smaller represented population\n"
        "  --all-people-coverage <0..1>  Explicitly enable coverage correction\n"
        "  --calibration-validated       Declare supplied estimator calibration validated\n"
        "  --address-rotation-validated  Declare address-rotation behavior calibrated\n"
        "  --residual-q-low <n> --residual-q-high <n>  Enable empirical uncertainty range\n"
        "  --residual-range-target <radio|all>  Target represented by the residual quantiles\n"
        "  --bt-transient-weight <0..1> --wifi-transient-weight <0..1>\n"
        "  --bt-persistence-seconds <s> --wifi-persistence-seconds <s>\n"
        "  --bt-persistence-sightings <n> --wifi-persistence-sightings <n>\n"
        "\n"
        "FILTER OPTIONS:\n"
        "  --rssi-min <dBm>    Ignore devices below this RSSI (default: -85)\n"
        "  --ttl <s>           Evict devices unseen for this duration (default: 60s)\n"
        "\n"
        "OUTPUT OPTIONS:\n"
        "  --verbose           Enable debug logging\n"
        "  --quiet             Suppress informational logs; show estimates only\n"
        "\n"
        "OTHER:\n"
        "  --help              Show this help text\n"
        "  --version           Show version\n"
        "\n"
        "IMPORTANT:\n"
        "  Wi-Fi live capture requires a monitor-capable adapter/driver and\n"
        "  CAP_NET_ADMIN plus CAP_NET_RAW (or root). Use --no-bt for Wi-Fi-only.\n"
        "  Estimator defaults are placeholders; outputs are unvalidated unless\n"
        "  calibration is supplied and explicitly declared validated.\n"
        "  WSL2 may not expose physical radio interfaces; --simulate is not live data.\n"
        "  --simulate mode is DETERMINISTIC; it does not represent real scans.\n",
        PROXSENSE_VERSION,
        program_name, program_name, program_name);
}

/* =========================================================================
 * Argument parsing helpers
 * ========================================================================= */

/**
 * Parse optarg as a positive integer.
 * Returns true on success and writes value to *out.
 * Prints an error message and returns false on failure.
 */
static bool parse_positive_int(const char *arg, const char *name, int *out)
{
    char *end;
    errno = 0;
    long val = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        fprintf(stderr, "Error: --%s: '%s' is not a valid integer.\n",
                name, arg);
        return false;
    }
    if (val <= 0 || val > INT_MAX) {
        fprintf(stderr, "Error: --%s: value %ld is out of range (must be > 0).\n",
                name, val);
        return false;
    }
    *out = (int)val;
    return true;
}

/**
 * Parse optarg as an integer in [-128, 0] (RSSI dBm range).
 * Returns true on success, false on invalid input.
 */
static bool parse_rssi(const char *arg, int8_t *out)
{
    char *end;
    errno = 0;
    long val = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0') {
        fprintf(stderr, "Error: --rssi-min: '%s' is not a valid integer.\n", arg);
        return false;
    }
    if (val < -128 || val > 0) {
        fprintf(stderr, "Error: --rssi-min: value %ld is out of range [-128, 0].\n", val);
        return false;
    }
    *out = (int8_t)val;
    return true;
}

/**
 * Parse optarg as a non-negative HCI device index (0, 1, 2 …).
 */
static bool parse_hci_index(const char *arg, int *out)
{
    char *end;
    errno = 0;
    long val = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || val < 0 || val > 255) {
        fprintf(stderr,
                "Error: --bt-hci: '%s' is not a valid HCI index (0-255).\n",
                arg);
        return false;
    }
    *out = (int)val;
    return true;
}

static bool parse_double_value(const char *arg, const char *name, double *out)
{
    char *end = NULL;
    errno = 0;
    double value = strtod(arg, &end);
    if (errno != 0 || end == arg || *end != '\0' || !isfinite(value)) {
        fprintf(stderr, "Error: --%s requires a finite number.\n", name);
        return false;
    }
    *out = value;
    return true;
}

/* =========================================================================
 * Main parse function
 * ========================================================================= */

ps_status_t ps_config_parse_args(int argc, char **argv, ps_config_t *out_cfg)
{
    if (!argv || !out_cfg) return PS_ERR_INVALID_ARG;

    *out_cfg = ps_config_default();

    static const struct option long_opts[] = {
        /* Run mode */
        {"live",            no_argument,       NULL, 'L'},
        {"simulate",        required_argument, NULL, 'S'},
        {"list-interfaces", no_argument,       NULL, 'l'},
        /* Scan duration & reporting */
        {"duration",        required_argument, NULL, 'd'},
        {"interval",        required_argument, NULL, 'i'},
        /* Scanner selection */
        {"bt-hci",          required_argument, NULL, 'b'},
        {"wifi-iface",      required_argument, NULL, 'w'},
        {"wifi-interface",  required_argument, NULL, 'w'},
        {"bt-records-per-person", required_argument, NULL, 'K'},
        {"wifi-records-per-person", required_argument, NULL, 'Y'},
        {"radio-overlap", required_argument, NULL, 'O'},
        {"all-people-coverage", required_argument, NULL, 'C'},
        {"calibration-validated", no_argument, NULL, 'Z'},
        {"address-rotation-validated", no_argument, NULL, 'A'},
        {"bt-transient-weight", required_argument, NULL, 'm'},
        {"wifi-transient-weight", required_argument, NULL, 'n'},
        {"bt-persistence-seconds", required_argument, NULL, 'p'},
        {"wifi-persistence-seconds", required_argument, NULL, 'P'},
        {"bt-persistence-sightings", required_argument, NULL, 'g'},
        {"wifi-persistence-sightings", required_argument, NULL, 'G'},
        {"residual-q-low", required_argument, NULL, 'x'},
        {"residual-q-high", required_argument, NULL, 'y'},
        {"residual-range-target", required_argument, NULL, 'z'},
        {"no-bt",           no_argument,       NULL, 'B'},
        {"no-wifi",         no_argument,       NULL, 'W'},
        /* Filtering */
        {"rssi-min",        required_argument, NULL, 'r'},
        {"ttl",             required_argument, NULL, 't'},
        /* Logging */
        {"verbose",         no_argument,       NULL, 'v'},
        {"quiet",           no_argument,       NULL, 'q'},
        /* Meta */
        {"help",            no_argument,       NULL, 'h'},
        {"version",         no_argument,       NULL, 'V'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    int opt_index  = 0;
    bool parse_ok  = true;
    bool residual_low_seen = false;
    bool residual_high_seen = false;
    bool residual_target_seen = false;
    bool bt_rate_seen = false;
    bool wifi_rate_seen = false;
    bool overlap_seen = false;

    while ((opt = getopt_long(argc, argv, "", long_opts, &opt_index)) != -1) {
        switch (opt) {

        /* --- Meta --- */
        case 'h':
            ps_config_print_help(argv[0]);
            exit(EXIT_SUCCESS);

        case 'V':
            printf("ProxSense %s\n", PROXSENSE_VERSION);
            exit(EXIT_SUCCESS);

        /* --- Run mode --- */
        case 'L':
            out_cfg->run_mode = PS_MODE_LIVE;
            break;

        case 'S':
            out_cfg->run_mode         = PS_MODE_SIMULATE;
            out_cfg->sim.scenario_file = optarg; /* string owned by argv */
            break;

        case 'l':
            out_cfg->run_mode = PS_MODE_LIST_IFACES;
            break;

        /* --- Duration & interval --- */
        case 'd':
            parse_ok = parse_positive_int(optarg, "duration",
                                          &out_cfg->duration_s);
            break;

        case 'i':
            parse_ok = parse_positive_int(optarg, "interval",
                                          &out_cfg->report_interval_s);
            break;

        /* --- Scanner selection --- */
        case 'b': {
            int hci_id = 0;
            parse_ok = parse_hci_index(optarg, &hci_id);
            if (parse_ok) out_cfg->bt.hci_dev_id = hci_id;
            break;
        }

        case 'w':
            if (optarg[0] == '\0' || strlen(optarg) >= PS_IFACE_NAME_LEN) {
                fprintf(stderr, "Error: --wifi-interface requires a non-empty Linux interface name of at most %d characters.\n",
                        PS_IFACE_NAME_LEN - 1);
                parse_ok = false;
                break;
            }
            (void)strcpy(out_cfg->wifi.iface, optarg);
            break;

        case 'K':
            parse_ok = parse_double_value(optarg, "bt-records-per-person",
                                          &out_cfg->estimator.bt_records_per_person);
            bt_rate_seen = parse_ok;
            break;

        case 'Y':
            parse_ok = parse_double_value(optarg, "wifi-records-per-person",
                                          &out_cfg->estimator.wifi_records_per_person);
            wifi_rate_seen = parse_ok;
            break;

        case 'O':
            parse_ok = parse_double_value(optarg, "radio-overlap",
                                          &out_cfg->estimator.overlap_fraction);
            overlap_seen = parse_ok;
            break;

        case 'C':
            parse_ok = parse_double_value(optarg, "all-people-coverage",
                                          &out_cfg->estimator.either_radio_coverage);
            if (parse_ok) out_cfg->estimator.all_people_coverage_enabled = true;
            break;

        case 'Z':
            out_cfg->estimator.calibration_validated = true;
            break;

        case 'A':
            out_cfg->estimator.address_rotation_validated = true;
            break;

        case 'm':
            parse_ok = parse_double_value(optarg, "bt-transient-weight",
                                          &out_cfg->estimator.bt_transient_weight);
            break;

        case 'n':
            parse_ok = parse_double_value(optarg, "wifi-transient-weight",
                                          &out_cfg->estimator.wifi_transient_weight);
            break;

        case 'p':
            parse_ok = parse_double_value(optarg, "bt-persistence-seconds",
                                          &out_cfg->estimator.bt_min_persistence_s);
            break;

        case 'P':
            parse_ok = parse_double_value(optarg, "wifi-persistence-seconds",
                                          &out_cfg->estimator.wifi_min_persistence_s);
            break;

        case 'g': {
            int sightings = 0;
            parse_ok = parse_positive_int(optarg, "bt-persistence-sightings", &sightings);
            if (parse_ok) out_cfg->estimator.bt_min_sightings = (uint32_t)sightings;
            break;
        }

        case 'G': {
            int sightings = 0;
            parse_ok = parse_positive_int(optarg, "wifi-persistence-sightings", &sightings);
            if (parse_ok) out_cfg->estimator.wifi_min_sightings = (uint32_t)sightings;
            break;
        }

        case 'x':
            parse_ok = parse_double_value(optarg, "residual-q-low",
                                          &out_cfg->estimator.residual_q_low);
            residual_low_seen = parse_ok;
            break;

        case 'y':
            parse_ok = parse_double_value(optarg, "residual-q-high",
                                          &out_cfg->estimator.residual_q_high);
            residual_high_seen = parse_ok;
            break;

        case 'z':
            if (strcmp(optarg, "radio") == 0) {
                out_cfg->estimator.residuals_for_all_people = false;
                residual_target_seen = true;
            } else if (strcmp(optarg, "all") == 0) {
                out_cfg->estimator.residuals_for_all_people = true;
                residual_target_seen = true;
            } else {
                fprintf(stderr, "Error: --residual-range-target must be 'radio' or 'all'.\n");
                parse_ok = false;
            }
            break;

        case 'B':
            out_cfg->enable_bt = false;
            break;

        case 'W':
            out_cfg->enable_wifi = false;
            break;

        /* --- Filtering --- */
        case 'r':
            parse_ok = parse_rssi(optarg, &out_cfg->filter.rssi_min_dbm);
            break;

        case 't': {
            int ttl = 0;
            parse_ok = parse_positive_int(optarg, "ttl", &ttl);
            if (parse_ok) out_cfg->filter.device_ttl_s = ttl;
            break;
        }

        /* --- Logging --- */
        case 'v':
            out_cfg->verbose = true;
            ps_log_set_level(PS_LOG_LEVEL_DEBUG);
            break;

        case 'q':
            out_cfg->quiet = true;
            ps_log_set_level(PS_LOG_LEVEL_ERROR);
            break;

        case '?':
            /* getopt_long already printed an error message. */
            ps_config_print_help(argv[0]);
            return PS_ERR_INVALID_ARG;

        default:
            fprintf(stderr, "Internal: unhandled option '%c'\n", opt);
            return PS_ERR_INVALID_ARG;
        }

        if (!parse_ok) {
            ps_config_print_help(argv[0]);
            return PS_ERR_INVALID_ARG;
        }
    }

    /* --- Cross-option validation --- */
    if (out_cfg->verbose && out_cfg->quiet) {
        fprintf(stderr, "Error: --verbose and --quiet are mutually exclusive.\n");
        return PS_ERR_INVALID_ARG;
    }

    if (out_cfg->run_mode == PS_MODE_SIMULATE &&
        !out_cfg->sim.scenario_file) {
        /*
         * --simulate was passed without a filename, which shouldn't happen
         * because getopt requires the argument.  Guard defensively.
         */
        fprintf(stderr, "Error: --simulate requires a scenario file path.\n");
        return PS_ERR_INVALID_ARG;
    }

    if (residual_low_seen != residual_high_seen) {
        fprintf(stderr, "Error: --residual-q-low and --residual-q-high must be supplied together.\n");
        return PS_ERR_INVALID_ARG;
    }
    if (residual_target_seen && !residual_low_seen) {
        fprintf(stderr, "Error: --residual-range-target requires both residual quantiles.\n");
        return PS_ERR_INVALID_ARG;
    }
    if (out_cfg->estimator.calibration_validated &&
        (!bt_rate_seen || !wifi_rate_seen || !overlap_seen)) {
        fprintf(stderr, "Error: --calibration-validated requires explicit BT rate, Wi-Fi rate, and radio-overlap values.\n");
        return PS_ERR_INVALID_ARG;
    }
    if (residual_low_seen) out_cfg->estimator.uncertainty_range_enabled = true;
    if (!ps_estimator_config_is_valid(&out_cfg->estimator)) {
        fprintf(stderr, "Error: invalid estimator calibration or persistence configuration.\n");
        return PS_ERR_INVALID_ARG;
    }

    return PS_OK;
}
