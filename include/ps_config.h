/**
 * @file ps_config.h
 * @brief Application-level configuration (CLI arguments + config file).
 *
 * Centralises all runtime-configurable parameters in one place.
 * The CLI parser fills a ps_config_t, which is then passed down to each
 * subsystem's init function.  No subsystem reads argv directly.
 */

#ifndef PS_CONFIG_H
#define PS_CONFIG_H

#include "ps_types.h"
#include "ps_obs_manager.h"
#include "ps_estimator.h"
#include "ps_bt_scanner.h"
#include "ps_wifi_scanner.h"
#include "ps_sim_scanner.h"
#include "ps_log.h"
#include <stdbool.h>

/* =========================================================================
 * Run mode — mutually exclusive
 * ========================================================================= */
typedef enum ps_run_mode {
    PS_MODE_LIVE      = 0, /**< Use real Bluetooth/Wi-Fi hardware              */
    PS_MODE_SIMULATE  = 1, /**< Replay a scenario file (no hardware required)  */
    PS_MODE_LIST_IFACES=2  /**< Print available BT/Wi-Fi interfaces and exit   */
} ps_run_mode_t;

/* =========================================================================
 * Top-level application configuration
 * ========================================================================= */
typedef struct ps_config {
    ps_run_mode_t        run_mode;       /**< How the application will run          */
    int                  duration_s;     /**< How long to scan; 0 = indefinite      */
    int                  report_interval_s; /**< How often to print an estimate     */
    bool                 verbose;        /**< Enable DEBUG log level                */
    bool                 quiet;          /**< Suppress INFO; show only estimates    */
    bool                 enable_bt;      /**< Whether to start the BT scanner       */
    bool                 enable_wifi;    /**< Whether to start the Wi-Fi scanner    */

    /* Per-scanner configuration */
    ps_bt_scanner_config_t   bt;
    ps_wifi_scanner_config_t wifi;
    ps_sim_scanner_config_t  sim;

    /* Pipeline configuration */
    ps_filter_config_t    filter;
    ps_estimator_config_t estimator;
} ps_config_t;

/**
 * @brief Parse command-line arguments into a ps_config_t.
 *
 * @param argc     Argument count from main().
 * @param argv     Argument vector from main().
 * @param out_cfg  Configuration structure to fill.
 * @return PS_OK, PS_ERR_INVALID_ARG (bad arguments), or PS_ERR_NOT_FOUND
 *         (e.g. --simulate file not found).  On error, usage is printed.
 */
ps_status_t ps_config_parse_args(int argc, char **argv, ps_config_t *out_cfg);

/**
 * @brief Return a configuration with all fields set to reasonable defaults.
 */
ps_config_t ps_config_default(void);

/**
 * @brief Print usage/help text to stdout.
 * Called when --help is passed or invalid arguments are given.
 */
void ps_config_print_help(const char *program_name);

#endif /* PS_CONFIG_H */
