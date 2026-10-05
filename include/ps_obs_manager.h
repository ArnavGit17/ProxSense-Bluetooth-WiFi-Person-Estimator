/**
 * @file ps_obs_manager.h
 * @brief Observation Manager — the central pipeline stage between scanners and estimation.
 *
 * Responsibilities (in order of execution):
 *
 *   1. RECEIVE raw observations from one or more scanner back-ends.
 *   2. FILTER observations below the configured RSSI threshold. Unavailable
 *      RSSI is accepted. Infrastructure and minimum-dwell filters are not
 *      implemented by the current manager.
 *   3. DEDUPLICATE: maintain a table of known devices keyed by MAC address.
 *        - On repeat sighting: update last_seen and RSSI.
 *        - On new sighting: add entry and record first_seen.
 *   4. EXPIRE: remove devices not seen within a configurable window (TTL).
 *        - Prevents departed devices from inflating the estimate indefinitely.
 *   5. PUBLISH the current device table to the estimator on request.
 *
 * What this module does NOT do:
 *   - It does not estimate people count — that is the estimator's job.
 *   - It does not know about BlueZ or nl80211 — it only sees ps_observation_t.
 *   - It does not print output — that is the CLI's job.
 *
 * Thread safety:
 *   The manager is single-threaded. The main loop polls scanner back-ends
 *   sequentially and sends all observations through this instance.
 */

#ifndef PS_OBS_MANAGER_H
#define PS_OBS_MANAGER_H

#include "ps_observation.h"
#include "ps_types.h"
#include <stddef.h>

/* Maximum number of distinct devices tracked simultaneously.
 * This bounds memory usage.  If the table is full, the oldest entry is evicted
 * (LRU policy) and a warning is logged.
 * Rationale for 512: a very crowded environment (concert, office building)
 * might have hundreds of devices visible; 512 is conservative overhead. */
#define PS_OBS_MANAGER_MAX_DEVICES  512

/* =========================================================================
 * Filtering configuration
 * ========================================================================= */
typedef struct ps_filter_config {
    int8_t  rssi_min_dbm;    /**< Ignore obs with RSSI below this (default: -85 dBm)    */
    int32_t device_ttl_s;    /**< Evict device unseen for this many seconds (default: 60) */
    int32_t min_duration_s;  /**< Reserved; current manager does not apply dwell filtering */
    bool    filter_random_bt;/**< Reserved; current manager does not filter random BT records */
} ps_filter_config_t;

/* =========================================================================
 * Tracked device record (internal to the manager, but exposed for estimator)
 * ========================================================================= */
typedef struct ps_device_record {
    ps_mac_addr_t   mac;
    ps_source_t     source;
    int8_t          rssi_last_dbm;   /**< Most recent RSSI reading           */
    int8_t          rssi_peak_dbm;   /**< Best (strongest) RSSI ever seen    */
    struct timespec first_seen;
    struct timespec last_seen;
    uint32_t        sighting_count;  /**< Number of times this MAC was seen  */
    bool            is_random_bt;    /**< True if BLE random address type    */
    char            name[PS_DEVICE_NAME_MAX_LEN]; /**< Device name if known  */
} ps_device_record_t;

/* =========================================================================
 * Observation Manager handle
 * ========================================================================= */
typedef struct ps_obs_manager {
    ps_filter_config_t  filter;                              /**< Active filter params */
    ps_device_record_t  table[PS_OBS_MANAGER_MAX_DEVICES];  /**< Flat device table    */
    size_t              table_count;                         /**< Current active count */
} ps_obs_manager_t;

/**
 * @brief Initialise an Observation Manager with the given filter configuration.
 * @param mgr     Manager to initialise (caller owns memory).
 * @param filter  Filter config; NULL uses built-in defaults.
 * @return PS_OK or PS_ERR_INVALID_ARG.
 */
ps_status_t ps_obs_manager_init(ps_obs_manager_t *mgr,
                                const ps_filter_config_t *filter);

/**
 * @brief Feed one observation into the manager.
 *
 * This is the hot path: called for every observation from every scanner.
 * It applies filtering, deduplication, and table update atomically.
 *
 * @param mgr  The manager.
 * @param obs  The observation (read-only; manager makes its own copy).
 * @return PS_OK, PS_ERR_INVALID_ARG, or PS_ERR_FULL (table overflow, eviction happened).
 */
ps_status_t ps_obs_manager_process(ps_obs_manager_t *mgr,
                                   const ps_observation_t *obs);

/**
 * @brief Expire stale device records older than filter.device_ttl_s.
 *
 * Should be called periodically (e.g. once per second) by the main loop.
 * Returns the number of records expired.
 */
size_t ps_obs_manager_expire(ps_obs_manager_t *mgr);

/**
 * @brief Provide a read-only snapshot of the current device table.
 *
 * Writes pointers into @p out_records (up to @p max_records entries).
 * Caller must not modify or free the returned records.
 *
 * @param mgr          The manager.
 * @param out_records  Caller-supplied array of const pointers to fill.
 * @param max_records  Length of out_records array.
 * @param out_count    Set to the number of records written.
 * @return PS_OK or PS_ERR_INVALID_ARG.
 */
ps_status_t ps_obs_manager_snapshot(const ps_obs_manager_t *mgr,
                                    const ps_device_record_t **out_records,
                                    size_t max_records,
                                    size_t *out_count);

/**
 * @brief Return the default filter configuration.
 */
ps_filter_config_t ps_filter_config_default(void);

#endif /* PS_OBS_MANAGER_H */
