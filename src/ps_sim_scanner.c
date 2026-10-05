/**
 * @file ps_sim_scanner.c
 * @brief Deterministic simulation scanner — Phase 1 implementation.
 *
 * This scanner replays a text scenario file through the ps_scanner_ops_t
 * interface. It is the ONLY scanner that works in WSL2 without hardware.
 *
 * Rules:
 *   - All simulated observations are clearly tagged [SIMULATION] in logs.
 *   - Simulation mode is mutually exclusive with live mode (enforced by CLI).
 *   - The sim scanner NEVER claims to represent real hardware data.
 *
 * Scenario file format (one observation per line, '#' = comment):
 *   <time_s>  <source>  <mac>  <rssi_or_?>  [name]
 *
 *   time_s   : relative seconds from scenario start (float, e.g. 1.500)
 *   source   : BT  = Bluetooth  |  WIFI = Wi-Fi
 *   mac      : colon-separated uppercase MAC (AA:BB:CC:DD:EE:FF)
 *   rssi     : integer dBm (e.g. -65) or '?' if unavailable
 *   name     : optional human-readable device name (no whitespace)
 *
 * Timing:
 *   If config.realtime == true, sim_read_next() blocks until the simulated
 *   event time has arrived relative to the open() call.  This lets the
 *   temporal tracking (TTL, min_duration) behave identically to live mode.
 *
 *   If config.realtime == false, events are delivered immediately —
 *   useful for unit tests that don't want to sleep.
 *
 * Built-in scenario (used when config.scenario_file == NULL):
 *   A minimal in-memory sequence for smoke-testing the pipeline without
 *   requiring any file on disk.
 */

#include "ps_sim_scanner.h"
#include "ps_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

/* =========================================================================
 * Built-in minimal scenario (used when no scenario file is given)
 * This is NOT real hardware data. It exercises the pipeline minimally.
 * ========================================================================= */

typedef struct built_in_event {
    double      time_s;
    ps_source_t source;
    const char *mac;
    int         rssi;        /* PS_RSSI_UNAVAILABLE if unknown */
    const char *name;
} built_in_event_t;

/* Two devices, simulating a single person with one BT device and one
 * Wi-Fi probe, appearing at different times, for 6 seconds. */
static const built_in_event_t k_builtin_events[] = {
    { 0.0,  PS_SOURCE_BLUETOOTH, "AA:BB:CC:DD:EE:01", -62, "Sim-Phone-BT"  },
    { 0.2,  PS_SOURCE_WIFI,      "11:22:33:44:55:01", -70, "Sim-Phone-WiFi" },
    { 1.0,  PS_SOURCE_BLUETOOTH, "AA:BB:CC:DD:EE:01", -63, "Sim-Phone-BT"  },
    { 1.2,  PS_SOURCE_WIFI,      "11:22:33:44:55:01", -71, "Sim-Phone-WiFi" },
    { 2.0,  PS_SOURCE_BLUETOOTH, "AA:BB:CC:DD:EE:01", -61, "Sim-Phone-BT"  },
    { 2.2,  PS_SOURCE_WIFI,      "11:22:33:44:55:01", -69, "Sim-Phone-WiFi" },
};
static const size_t k_builtin_count =
    sizeof(k_builtin_events) / sizeof(k_builtin_events[0]);

/* =========================================================================
 * Private context
 * ========================================================================= */

typedef struct sim_priv {
    ps_sim_scanner_config_t config;

    /* File-based playback */
    FILE *scenario_fp;

    /* Built-in playback */
    bool   using_builtin;
    size_t builtin_idx;     /* next event index in k_builtin_events */

    /* Timing: absolute monotonic time when open() was called */
    struct timespec open_time;
} sim_priv_t;

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/** Sleep until at least `target` seconds after the scan opened. */
static void wait_until_offset(const sim_priv_t *priv, double target_s)
{
    if (!priv->config.realtime || target_s <= 0.0) return;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Compute how long since open(). */
    double elapsed = (double)(now.tv_sec  - priv->open_time.tv_sec) +
                     (double)(now.tv_nsec - priv->open_time.tv_nsec) / 1.0e9;
    double to_wait = target_s - elapsed;

    if (to_wait <= 0.0) return; /* Event is already due or past. */

    struct timespec sleep_ts = {
        .tv_sec  = (time_t)to_wait,
        .tv_nsec = (long)((to_wait - (double)(time_t)to_wait) * 1.0e9)
    };
    nanosleep(&sleep_ts, NULL);
}

/**
 * Parse one text line from the scenario file into a ps_observation_t.
 *
 * Line format:  <time_s>  <source>  <mac>  <rssi|?>  [name]
 * Lines starting with '#' or empty lines are skipped.
 *
 * @return true  if an observation was successfully parsed.
 * @return false if line is a comment/blank or malformed (logged).
 */
static bool parse_scenario_line(const char         *line,
                                double             *out_time_s,
                                ps_observation_t   *out_obs)
{
    /* Skip leading whitespace. */
    while (*line == ' ' || *line == '\t') line++;

    /* Skip blank lines and comments. */
    if (*line == '\0' || *line == '#' || *line == '\n') return false;

    /* Parse fields: time_s  SOURCE  MAC  rssi [name] */
    char source_str[8]  = {0};
    char mac_str[PS_MAC_STR_LEN] = {0};
    char rssi_str[8]    = {0};
    char name_str[PS_DEVICE_NAME_MAX_LEN] = {0};

    int n = sscanf(line, "%lf %7s %17s %7s %63s",
                   out_time_s, source_str, mac_str, rssi_str, name_str);

    if (n < 4) {
        PS_LOG_WARN("[SIMULATION] Malformed line (expected >=4 fields): %s", line);
        return false;
    }

    ps_observation_init(out_obs);

    /* Source */
    if (strcmp(source_str, "BT") == 0) {
        out_obs->source = PS_SOURCE_BLUETOOTH;
    } else if (strcmp(source_str, "WIFI") == 0) {
        out_obs->source = PS_SOURCE_WIFI;
    } else {
        PS_LOG_WARN("[SIMULATION] Unknown source '%s' on line: %s",
                    source_str, line);
        return false;
    }

    /* MAC — copy verbatim; validation is by length only */
    if (strlen(mac_str) != PS_MAC_STR_LEN - 1) {
        PS_LOG_WARN("[SIMULATION] Bad MAC '%s' on line: %s", mac_str, line);
        return false;
    }
    strncpy(out_obs->mac.str, mac_str, PS_MAC_STR_LEN - 1);
    out_obs->mac.str[PS_MAC_STR_LEN - 1] = '\0';

    /* RSSI or '?' */
    if (rssi_str[0] == '?') {
        out_obs->rssi_dbm = PS_RSSI_UNAVAILABLE;
    } else {
        long rssi_val = strtol(rssi_str, NULL, 10);
        if (rssi_val < INT8_MIN || rssi_val > INT8_MAX) {
            PS_LOG_WARN("[SIMULATION] RSSI out of int8 range: %s", rssi_str);
            out_obs->rssi_dbm = PS_RSSI_UNAVAILABLE;
        } else {
            out_obs->rssi_dbm = (int8_t)rssi_val;
        }
    }

    /* Optional name */
    if (n >= 5 && name_str[0] != '\0') {
        strncpy(out_obs->name, name_str, PS_DEVICE_NAME_MAX_LEN - 1);
        out_obs->name[PS_DEVICE_NAME_MAX_LEN - 1] = '\0';
    }

    /* Set Wi-Fi probe-request frame type by default for simulation. */
    if (out_obs->source == PS_SOURCE_WIFI) {
        out_obs->meta.wifi.frame_type = PS_WIFI_FRAME_PROBE_REQ;
    }

    return true;
}

/* =========================================================================
 * Scanner vtable implementations
 * ========================================================================= */

static ps_status_t sim_open(ps_scanner_t *s);
static ps_status_t sim_read_next(ps_scanner_t *s, ps_observation_t *out,
                                  int timeout_ms);
static void        sim_close(ps_scanner_t *s);
static const char *sim_describe(const ps_scanner_t *s);

static const ps_scanner_ops_t sim_ops = {
    .open       = sim_open,
    .read_next  = sim_read_next,
    .close      = sim_close,
    .describe   = sim_describe
};

static ps_status_t sim_open(ps_scanner_t *s)
{
    sim_priv_t *priv = s->priv;

    if (priv->config.scenario_file) {
        priv->scenario_fp = fopen(priv->config.scenario_file, "r");
        if (!priv->scenario_fp) {
            PS_LOG_ERROR("[SIMULATION] Cannot open scenario file '%s': %s",
                         priv->config.scenario_file, strerror(errno));
            return PS_ERR_IO;
        }
        PS_LOG_INFO("[SIMULATION] Loaded scenario: %s",
                    priv->config.scenario_file);
    } else {
        /* Use the built-in minimal scenario. */
        priv->using_builtin = true;
        priv->builtin_idx   = 0;
        PS_LOG_INFO("[SIMULATION] Using built-in scenario (%zu events).",
                    k_builtin_count);
    }

    /* Record the time at which the scan "started". */
    clock_gettime(CLOCK_MONOTONIC, &priv->open_time);

    s->is_open = true;
    return PS_OK;
}

static ps_status_t sim_read_next(ps_scanner_t    *s,
                                  ps_observation_t *out,
                                  int               timeout_ms)
{
    (void)timeout_ms; /* Simulation uses its own timing model. */

    sim_priv_t *priv = s->priv;

    if (!s->is_open) return PS_ERR_IO;

    if (priv->using_builtin) {
        /* Built-in scenario playback. */
        if (priv->builtin_idx >= k_builtin_count) {
            return PS_ERR_TIMEOUT; /* Scenario exhausted — signals end-of-data */
        }

        const built_in_event_t *ev = &k_builtin_events[priv->builtin_idx];
        wait_until_offset(priv, ev->time_s);

        ps_observation_init(out);
        out->source   = ev->source;
        out->rssi_dbm = (ev->rssi == PS_RSSI_UNAVAILABLE)
                        ? PS_RSSI_UNAVAILABLE
                        : (int8_t)ev->rssi;

        strncpy(out->mac.str, ev->mac, PS_MAC_STR_LEN - 1);
        out->mac.str[PS_MAC_STR_LEN - 1] = '\0';

        if (ev->name) {
            strncpy(out->name, ev->name, PS_DEVICE_NAME_MAX_LEN - 1);
            out->name[PS_DEVICE_NAME_MAX_LEN - 1] = '\0';
        }

        if (out->source == PS_SOURCE_WIFI) {
            out->meta.wifi.frame_type = PS_WIFI_FRAME_PROBE_REQ;
        }

        priv->builtin_idx++;
        return PS_OK;
    }

    /* File-based playback. */
    if (!priv->scenario_fp) return PS_ERR_IO;

    char line[256];
    double time_s = 0.0;

    while (fgets(line, sizeof(line), priv->scenario_fp)) {
        if (parse_scenario_line(line, &time_s, out)) {
            wait_until_offset(priv, time_s);
            return PS_OK;
        }
        /* Comment or blank line — read the next one. */
    }

    /* End of file. */
    return PS_ERR_TIMEOUT;
}

static void sim_close(ps_scanner_t *s)
{
    if (!s) return;
    sim_priv_t *priv = s->priv;
    if (priv) {
        if (priv->scenario_fp) {
            fclose(priv->scenario_fp);
            priv->scenario_fp = NULL;
        }
        free(priv);
        s->priv = NULL;
    }
    s->is_open = false;
    PS_LOG_INFO("[SIMULATION] Scanner closed.");
}

static const char *sim_describe(const ps_scanner_t *s)
{
    (void)s;
    return "Deterministic simulation scanner (no hardware — SIMULATION ONLY)";
}

/* =========================================================================
 * Public init function
 * ========================================================================= */

ps_status_t ps_sim_scanner_init(ps_scanner_t                  *scanner,
                                const ps_sim_scanner_config_t *config)
{
    if (!scanner) return PS_ERR_INVALID_ARG;

    sim_priv_t *priv = calloc(1, sizeof(sim_priv_t));
    if (!priv) return PS_ERR_ALLOC;

    if (config) {
        priv->config = *config;
    } else {
        /* Default: use built-in scenario, non-realtime (instant delivery). */
        priv->config.scenario_file = NULL;
        priv->config.realtime      = false;
    }

    scanner->ops     = &sim_ops;
    scanner->priv    = priv;
    scanner->source  = PS_SOURCE_UNKNOWN; /* produces both BT and Wi-Fi */
    scanner->is_open = false;

    PS_LOG_DEBUG("[SIMULATION] Scanner initialised (file=%s, realtime=%s)",
                 priv->config.scenario_file
                     ? priv->config.scenario_file
                     : "<built-in>",
                 priv->config.realtime ? "yes" : "no");
    return PS_OK;
}
