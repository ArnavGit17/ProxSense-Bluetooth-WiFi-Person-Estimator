/**
 * @file ps_observation.c
 * @brief Helper functions for the observation type.
 *
 * Phase 0: stub implementations providing string conversion utilities.
 * The scanner back-ends (ps_bt_scanner.c, ps_wifi_scanner.c) will be
 * implemented in Phase 1.
 */

#include "ps_observation.h"
#include <string.h>

const char *ps_source_name(ps_source_t source)
{
    switch (source) {
        case PS_SOURCE_BLUETOOTH: return "Bluetooth";
        case PS_SOURCE_WIFI:      return "Wi-Fi";
        case PS_SOURCE_UNKNOWN:   /* fall-through */
        default:                  return "Unknown";
    }
}

const char *ps_status_str(ps_status_t status)
{
    switch (status) {
        case PS_OK:                return "OK";
        case PS_ERR_INVALID_ARG:   return "Invalid argument";
        case PS_ERR_NOT_FOUND:     return "Not found";
        case PS_ERR_ALLOC:         return "Allocation failure";
        case PS_ERR_IO:            return "I/O error";
        case PS_ERR_PERMISSION:    return "Permission denied";
        case PS_ERR_NO_HARDWARE:   return "No hardware adapter";
        case PS_ERR_TIMEOUT:       return "Timeout";
        case PS_ERR_MALFORMED:     return "Malformed data";
        case PS_ERR_UNSUPPORTED:   return "Unsupported";
        case PS_ERR_FULL:          return "Buffer full";
        case PS_ERR_SCANNER_FAILED:return "Scanner failure";
        default:                   return "Unknown error";
    }
}
