/**
 * @file ps_observation.h
 * @brief The canonical device observation — the primary data unit of ProxSense.
 *
 * An "observation" is a single detection event: one radio technology saw one
 * device address at one point in time.  Observations are produced by scanner
 * back-ends and consumed by the Observation Manager for filtering, dedup, and
 * temporal tracking before reaching the estimator.
 *
 * Design note: This struct is kept as a plain-old-data value type. Scanners
 * fill one in and pass it by value (or const pointer) into the pipeline.
 * There is no heap allocation inside an observation itself.
 */

#ifndef PS_OBSERVATION_H
#define PS_OBSERVATION_H

#include "ps_types.h"
#include <stdint.h>
#include <time.h>

/* Maximum length of an optional human-readable device name.
 * Bluetooth device names can be up to 248 bytes (Core Spec §Vol 3 Part C §3.2.2).
 * We cap at 64 to keep the struct compact; longer names are truncated safely. */
#define PS_DEVICE_NAME_MAX_LEN  64

/* =========================================================================
 * Bluetooth-specific metadata
 * ========================================================================= */
typedef enum ps_bt_addr_type {
    PS_BT_ADDR_PUBLIC    = 0, /**< Classic BR/EDR or LE public address       */
    PS_BT_ADDR_RANDOM    = 1, /**< LE random address (may rotate frequently) */
    PS_BT_ADDR_UNKNOWN   = 2  /**< Address type not determined               */
} ps_bt_addr_type_t;

typedef struct ps_bt_meta {
    ps_bt_addr_type_t addr_type;  /**< Public vs. random LE address          */
    bool              is_classic; /**< True = BR/EDR, False = BLE            */
    uint8_t           adv_type;   /**< BLE advertising PDU type (0 if N/A)   */
} ps_bt_meta_t;

/* =========================================================================
 * Wi-Fi-specific metadata
 * ========================================================================= */
typedef enum ps_wifi_frame_type {
    PS_WIFI_FRAME_PROBE_REQ  = 0, /**< Client probing for APs (best signal)  */
    PS_WIFI_FRAME_PROBE_RESP = 1, /**< AP responding to a probe request      */
    PS_WIFI_FRAME_BEACON     = 2, /**< AP periodic beacon                    */
    PS_WIFI_FRAME_OTHER      = 3  /**< Data/management frame, unclassified   */
} ps_wifi_frame_type_t;

typedef struct ps_wifi_meta {
    ps_wifi_frame_type_t frame_type; /**< What kind of Wi-Fi frame was seen  */
    uint8_t              channel;    /**< 802.11 channel number (1-14, etc.) */
    char                 ssid[33];   /**< SSID if available, NUL if absent   */
    bool                 ssid_hidden;/**< True if SSID was not broadcast     */
    bool                 locally_administered; /**< Address bit; not proof of randomization */
} ps_wifi_meta_t;

/* =========================================================================
 * Source-specific metadata union
 *
 * Only the member corresponding to obs.source is valid. The union avoids a
 * heap allocation while keeping the struct self-contained.
 * ========================================================================= */
typedef union ps_source_meta {
    ps_bt_meta_t   bt;   /**< Valid when source == PS_SOURCE_BLUETOOTH */
    ps_wifi_meta_t wifi; /**< Valid when source == PS_SOURCE_WIFI       */
} ps_source_meta_t;

/* =========================================================================
 * ps_observation_t — the canonical observation record
 *
 * This is the unit of exchange between scanner back-ends and the rest of the
 * pipeline.  Keep it plain-old-data so it can be copied cheaply and stored
 * in a flat ring buffer without pointer chasing.
 * ========================================================================= */
typedef struct ps_observation {
    /* --- Identity --- */
    ps_source_t     source;                      /**< BT or Wi-Fi                      */
    ps_mac_addr_t   mac;                         /**< Normalised MAC address string     */
    char            name[PS_DEVICE_NAME_MAX_LEN];/**< Human name if available, else ""  */

    /* --- Signal strength --- */
    int8_t          rssi_dbm;  /**< Received Signal Strength Indicator in dBm.
                                    Use PS_RSSI_UNAVAILABLE when not provided. */

    /* --- Timing --- */
    struct timespec first_seen; /**< Monotonic clock when first seen this session */
    struct timespec last_seen;  /**< Monotonic clock of this specific sighting    */

    /* --- Source-specific metadata --- */
    ps_source_meta_t meta; /**< Interpret according to .source field              */
} ps_observation_t;

/* =========================================================================
 * Helper: zero-initialise an observation safely
 * ========================================================================= */
static inline void ps_observation_init(ps_observation_t *obs)
{
    /* Using a compound literal to zero all fields avoids partial init bugs. */
    *obs = (ps_observation_t){
        .source   = PS_SOURCE_UNKNOWN,
        .rssi_dbm = PS_RSSI_UNAVAILABLE
    };
}

/* Return a human-readable string for a source enum (for logging). */
const char *ps_source_name(ps_source_t source);

/* Return a human-readable string for a ps_status_t (for diagnostics). */
const char *ps_status_str(ps_status_t status);

#endif /* PS_OBSERVATION_H */
