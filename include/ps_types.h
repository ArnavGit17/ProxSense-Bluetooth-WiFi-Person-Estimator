/**
 * @file ps_types.h
 * @brief Core types, constants, and return codes shared across all ProxSense modules.
 *
 * This header is intentionally minimal. It defines only primitive vocabulary
 * types needed by multiple modules. Do not add module-specific logic here.
 *
 * C11 is the target standard throughout this project.
 */

#ifndef PS_TYPES_H
#define PS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <time.h>

/* =========================================================================
 * Project-wide return codes
 *
 * Every public function that can fail returns ps_status_t.
 * PS_OK == 0 intentionally, so callers can write: if (fn() != PS_OK) { ... }
 * ========================================================================= */
typedef enum ps_status {
    PS_OK                   =  0, /**< Success                              */
    PS_ERR_INVALID_ARG      = -1, /**< NULL or out-of-range argument        */
    PS_ERR_NOT_FOUND        = -2, /**< Requested resource does not exist    */
    PS_ERR_ALLOC            = -3, /**< Memory allocation failure            */
    PS_ERR_IO               = -4, /**< I/O or socket error                  */
    PS_ERR_PERMISSION       = -5, /**< Insufficient privileges (e.g. CAP_NET_RAW) */
    PS_ERR_NO_HARDWARE      = -6, /**< Adapter/interface not present        */
    PS_ERR_TIMEOUT          = -7, /**< Operation did not complete in time   */
    PS_ERR_MALFORMED        = -8, /**< Data was structurally invalid        */
    PS_ERR_UNSUPPORTED      = -9, /**< Feature not available in this build  */
    PS_ERR_FULL             = -10,/**< Internal buffer/table is at capacity */
    PS_ERR_SCANNER_FAILED   = -11 /**< Scanner reported a fatal error       */
} ps_status_t;

/* =========================================================================
 * Signal source
 *
 * Identifies which radio technology produced an observation.
 * A future NFC or UWB back-end would extend this enum.
 * ========================================================================= */
typedef enum ps_source {
    PS_SOURCE_UNKNOWN    = 0,
    PS_SOURCE_BLUETOOTH  = 1,
    PS_SOURCE_WIFI       = 2
} ps_source_t;

/* =========================================================================
 * RSSI sentinel
 *
 * RSSI is optional. Not all scan back-ends return it, and a reading of 0 dBm
 * is a valid (very close) measurement. Use this sentinel to distinguish
 * "no reading available" from a real zero.
 * ========================================================================= */
#define PS_RSSI_UNAVAILABLE  (INT8_MIN)   /* = -128, below any real reading */

/* =========================================================================
 * Device identifier
 *
 * Stored as a normalised 17-character ASCII string: "XX:XX:XX:XX:XX:XX\0"
 * Using a fixed string rather than a packed uint64_t keeps the code readable
 * and simplifies logging without sacrificing any functional properties.
 * ========================================================================= */
#define PS_MAC_STR_LEN   18   /* 17 chars + NUL terminator */

typedef struct ps_mac_addr {
    char str[PS_MAC_STR_LEN]; /**< Canonical upper-case colon-separated MAC */
} ps_mac_addr_t;

/* =========================================================================
 * Compile-time assertions (C11)
 * ========================================================================= */
_Static_assert(sizeof(ps_mac_addr_t) == PS_MAC_STR_LEN,
               "ps_mac_addr_t size mismatch");

#endif /* PS_TYPES_H */
