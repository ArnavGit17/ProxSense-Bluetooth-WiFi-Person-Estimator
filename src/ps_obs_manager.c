/**
 * @file ps_obs_manager.c
 * @brief Observation Manager — Phase 1 implementation.
 *
 * Responsibilities implemented in this phase:
 *
 *   1. FILTER  — Reject observations below the RSSI threshold.
 *                RSSI unavailable (PS_RSSI_UNAVAILABLE) is always accepted.
 *
 *   2. DEDUPLICATE — Maintain a flat table keyed by (source, MAC).
 *                    On new observation: insert entry.
 *                    On repeat observation: update last_seen, RSSI, sighting_count.
 *
 *   3. TTL EXPIRY — Remove entries not seen within filter.device_ttl_s seconds.
 *                   Called by the main loop periodically.
 *
 *   4. SNAPSHOT — Provide read-only pointers into the table for the estimator.
 *
 * What this module does NOT do:
 *   - Does not estimate people count.
 *   - Does not correlate Bluetooth and Wi-Fi records across radios.
 *   - Does not distinguish people from devices.
 *
 * Table overflow policy:
 *   If the table reaches PS_OBS_MANAGER_MAX_DEVICES, the entry with the
 *   oldest last_seen timestamp is evicted (LRU) to make room, and a warning
 *   is logged.  This is safe for typical deployments; 512 slots covers even
 *   very dense environments.
 *
 * Monotonic clock:
 *   CLOCK_MONOTONIC is used for all internal timing so that system clock
 *   adjustments (NTP, DST) do not affect expiry logic.
 */

#include "ps_obs_manager.h"
#include "ps_log.h"
#include <string.h>
#include <time.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/**
 * Return the difference in seconds between two struct timespec values.
 * Result is negative if b is earlier than a.
 */
static double timespec_diff_s(const struct timespec *a, const struct timespec *b)
{
    return (double)(b->tv_sec  - a->tv_sec) +
           (double)(b->tv_nsec - a->tv_nsec) / 1.0e9;
}

/** Get the current monotonic time. */
static void monotonic_now(struct timespec *ts)
{
    clock_gettime(CLOCK_MONOTONIC, ts);
}

/**
 * Find the index of an existing record matching (source, mac).
 * Returns index in [0, mgr->table_count) if found, or -1 if not.
 */
static int find_record(const ps_obs_manager_t *mgr,
                       ps_source_t            source,
                       const ps_mac_addr_t   *mac)
{
    for (size_t i = 0; i < mgr->table_count; ++i) {
        if (mgr->table[i].source == source &&
            strncmp(mgr->table[i].mac.str, mac->str, PS_MAC_STR_LEN) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Find the index of the least-recently-seen entry in the table.
 * Used for LRU eviction when the table is full.
 * Assumes table_count > 0.
 */
static size_t find_lru_index(const ps_obs_manager_t *mgr)
{
    size_t lru_idx = 0;
    for (size_t i = 1; i < mgr->table_count; ++i) {
        /* Compare seconds first; only compare nanoseconds on a tie. */
        if (mgr->table[i].last_seen.tv_sec < mgr->table[lru_idx].last_seen.tv_sec ||
            (mgr->table[i].last_seen.tv_sec  == mgr->table[lru_idx].last_seen.tv_sec &&
             mgr->table[i].last_seen.tv_nsec <  mgr->table[lru_idx].last_seen.tv_nsec)) {
            lru_idx = i;
        }
    }
    return lru_idx;
}

/**
 * Remove the entry at position idx by replacing it with the last entry.
 * O(1) removal; does not preserve table order (order is not required).
 */
static void remove_at(ps_obs_manager_t *mgr, size_t idx)
{
    if (mgr->table_count == 0) return;
    mgr->table[idx] = mgr->table[mgr->table_count - 1];
    /* Zero-clear the now-unused trailing slot. */
    memset(&mgr->table[mgr->table_count - 1], 0, sizeof(ps_device_record_t));
    mgr->table_count--;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

ps_filter_config_t ps_filter_config_default(void)
{
    /*
     * These thresholds are conservative placeholders, NOT calibrated values.
     * They will be refined in Phase 2 based on validation experiments.
     * See docs/VALIDATION.md for the planned calibration process.
     */
    return (ps_filter_config_t){
        .rssi_min_dbm     = -85,  /* dBm: ignore devices likely > 20 m away */
        .device_ttl_s     =  60,  /* seconds: evict unseen devices after 1 min */
        .min_duration_s   =   5,  /* reserved; not applied by current manager */
        .filter_random_bt = true  /* reserved; not applied by current manager */
    };
}

ps_status_t ps_obs_manager_init(ps_obs_manager_t         *mgr,
                                const ps_filter_config_t *filter)
{
    if (!mgr) return PS_ERR_INVALID_ARG;

    memset(mgr, 0, sizeof(*mgr));
    mgr->filter = filter ? *filter : ps_filter_config_default();

    PS_LOG_INFO("Observation manager initialised (RSSI min=%d dBm, TTL=%d s)",
                (int)mgr->filter.rssi_min_dbm, mgr->filter.device_ttl_s);
    return PS_OK;
}

ps_status_t ps_obs_manager_process(ps_obs_manager_t      *mgr,
                                   const ps_observation_t *obs)
{
    if (!mgr || !obs) return PS_ERR_INVALID_ARG;

    /* --- Filter 1: RSSI threshold ---
     * Accept unavailable RSSI (sensor didn't report it) but reject readings
     * that are below the configured minimum.  The threshold is a rough
     * distance proxy; it is not a precise measurement. */
    if (obs->rssi_dbm != PS_RSSI_UNAVAILABLE &&
        obs->rssi_dbm < mgr->filter.rssi_min_dbm) {
        PS_LOG_DEBUG("Filtered (RSSI %d dBm < min %d): %s",
                     (int)obs->rssi_dbm,
                     (int)mgr->filter.rssi_min_dbm,
                     obs->mac.str);
        return PS_OK; /* Filtered — not an error */
    }

    struct timespec now;
    monotonic_now(&now);

    /* --- Deduplication: find existing record --- */
    int idx = find_record(mgr, obs->source, &obs->mac);

    if (idx >= 0) {
        /* Update existing record */
        ps_device_record_t *rec = &mgr->table[(size_t)idx];
        rec->last_seen     = now;
        rec->sighting_count++;

        if (obs->rssi_dbm != PS_RSSI_UNAVAILABLE) {
            rec->rssi_last_dbm = obs->rssi_dbm;
            if (obs->rssi_dbm > rec->rssi_peak_dbm) {
                rec->rssi_peak_dbm = obs->rssi_dbm;
            }
        }

        /* Update name if this observation provides one and we don't have it. */
        if (obs->name[0] != '\0' && rec->name[0] == '\0') {
            strncpy(rec->name, obs->name, PS_DEVICE_NAME_MAX_LEN - 1);
            rec->name[PS_DEVICE_NAME_MAX_LEN - 1] = '\0';
        }

        PS_LOG_DEBUG("Updated record: src=%s mac=%s rssi=%d sightings=%u",
                     ps_source_name(obs->source),
                     obs->mac.str,
                     (int)obs->rssi_dbm,
                     rec->sighting_count);
        return PS_OK;
    }

    /* --- New device: make room if needed --- */
    ps_status_t result = PS_OK;
    if (mgr->table_count >= PS_OBS_MANAGER_MAX_DEVICES) {
        size_t lru = find_lru_index(mgr);
        PS_LOG_WARN("Table full (%d slots). Evicting LRU entry: %s",
                    PS_OBS_MANAGER_MAX_DEVICES,
                    mgr->table[lru].mac.str);
        remove_at(mgr, lru);
        result = PS_ERR_FULL; /* Caller may log a warning but we continue */
    }

    /* --- Insert new record --- */
    ps_device_record_t *rec = &mgr->table[mgr->table_count];
    rec->mac          = obs->mac;
    rec->source       = obs->source;
    rec->rssi_last_dbm = obs->rssi_dbm;
    rec->rssi_peak_dbm = obs->rssi_dbm;
    rec->first_seen   = now;
    rec->last_seen    = now;
    rec->sighting_count = 1;

    /* Preserve BLE random-address classification for estimator diagnostics. */
    if (obs->source == PS_SOURCE_BLUETOOTH &&
        obs->meta.bt.addr_type == PS_BT_ADDR_RANDOM) {
        rec->is_random_bt = true;
    }

    if (obs->name[0] != '\0') {
        strncpy(rec->name, obs->name, PS_DEVICE_NAME_MAX_LEN - 1);
        rec->name[PS_DEVICE_NAME_MAX_LEN - 1] = '\0';
    }

    mgr->table_count++;

    PS_LOG_DEBUG("New device: src=%s mac=%s rssi=%d (table=%zu)",
                 ps_source_name(obs->source),
                 obs->mac.str,
                 (int)obs->rssi_dbm,
                 mgr->table_count);

    return result;
}

size_t ps_obs_manager_expire(ps_obs_manager_t *mgr)
{
    if (!mgr) return 0;

    struct timespec now;
    monotonic_now(&now);

    size_t expired = 0;
    size_t i = 0;
    while (i < mgr->table_count) {
        double age_s = timespec_diff_s(&mgr->table[i].last_seen, &now);
        if (age_s >= (double)mgr->filter.device_ttl_s) {
            PS_LOG_DEBUG("Expiring device: %s (age=%.1f s)",
                         mgr->table[i].mac.str, age_s);
            remove_at(mgr, i);
            expired++;
            /* Do NOT increment i — the entry at position i is now the
             * element that was previously at table_count-1. */
        } else {
            i++;
        }
    }

    if (expired > 0) {
        PS_LOG_DEBUG("Expired %zu device record(s). Active: %zu",
                     expired, mgr->table_count);
    }

    return expired;
}

ps_status_t ps_obs_manager_snapshot(const ps_obs_manager_t    *mgr,
                                    const ps_device_record_t **out_records,
                                    size_t                      max_records,
                                    size_t                     *out_count)
{
    if (!mgr || !out_records || !out_count) return PS_ERR_INVALID_ARG;

    size_t n = mgr->table_count < max_records ? mgr->table_count : max_records;
    for (size_t i = 0; i < n; ++i) {
        out_records[i] = &mgr->table[i];
    }
    *out_count = n;

    PS_LOG_DEBUG("Snapshot: %zu/%zu records provided (table has %zu)",
                 n, max_records, mgr->table_count);
    return PS_OK;
}
