/**
 * @file ps_sim_scanner.h
 * @brief Deterministic simulation scanner back-end.
 *
 * Purpose:
 *   Provides a ps_scanner_t implementation that replays a pre-recorded or
 *   script-defined sequence of observations.  It implements exactly the same
 *   ps_scanner_ops_t interface as the real Bluetooth and Wi-Fi back-ends, so
 *   the entire pipeline (filtering, dedup, tracking, estimation) can be
 *   exercised without any hardware.
 *
 * Design rules:
 *   1. This module NEVER claims to produce real hardware observations.
 *   2. All observations it produces carry ps_source_t = PS_SOURCE_BLUETOOTH or
 *      PS_SOURCE_WIFI, but are read from a scenario file or in-memory script.
 *   3. The CLI marks simulation output clearly to prevent misinterpretation.
 *   4. Simulation mode CANNOT be enabled simultaneously with live scanning.
 *
 * Scenario file format (text, described in docs/VALIDATION.md):
 *   Each non-comment line defines one observation event:
 *     <timestamp_s> <source> <mac> <rssi> [name]
 *   Example:
 *     0.000  BT   AA:BB:CC:DD:EE:FF  -65  MyPhone
 *     0.500  WIFI 11:22:33:44:55:66  -72
 *     1.200  BT   AA:BB:CC:DD:EE:FF  -66  MyPhone
 *
 * The simulator delivers observations at the real-time pace of their
 * timestamps (sleeping between events) so the temporal tracking logic
 * behaves identically to live mode.
 *
 * WSL2 note:
 *   This is the ONLY scanner mode that works in WSL2 without hardware.
 *   All automated tests use this back-end.
 */

#ifndef PS_SIM_SCANNER_H
#define PS_SIM_SCANNER_H

#include "ps_scanner.h"

/* Configuration for the simulation scanner. */
typedef struct ps_sim_scanner_config {
    const char *scenario_file; /**< Path to scenario file; NULL = use default built-in */
    bool        realtime;      /**< If true, sleep between events to match timestamps  */
                               /**< If false, deliver all events immediately (for tests)*/
} ps_sim_scanner_config_t;

/**
 * @brief Initialise the deterministic simulation scanner.
 *
 * @param scanner  Caller-allocated scanner instance.
 * @param config   Simulation parameters.
 * @return PS_OK or PS_ERR_INVALID_ARG / PS_ERR_ALLOC / PS_ERR_IO.
 */
ps_status_t ps_sim_scanner_init(ps_scanner_t *scanner,
                                const ps_sim_scanner_config_t *config);

#endif /* PS_SIM_SCANNER_H */
