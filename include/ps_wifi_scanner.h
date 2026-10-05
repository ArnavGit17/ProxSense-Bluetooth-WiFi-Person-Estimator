/**
 * @file ps_wifi_scanner.h
 * @brief Linux nl80211 monitor-mode Wi-Fi scanner interface.
 *
 * nl80211 configures or inspects the selected wireless interface. AF_PACKET
 * captures radiotap-framed 802.11 traffic; only valid Probe Request frames
 * become observations. The scanner never falls back to managed-mode AP scans.
 *
 * Interface selection is optional. An empty name selects the first wireless
 * interface exposed through Linux sysfs; --wifi-interface selects one by name.
 * A virtual monitor interface is requested when required and removed on close.
 * Capture remains on the PHY's current channel; channel hopping is not done.
 *
 * Probe Requests are useful but incomplete: devices may not emit them while
 * nearby, may be asleep, or may suppress them for privacy. Locally administered
 * MAC addresses are only a hint and may rotate; MACs do not identify people or
 * provide reliable Bluetooth-to-Wi-Fi correlation.
 *
 * RSSI is populated only when radiotap provides a valid dBm signal field.
 * Monitor support and permissions depend on the adapter, driver, kernel, and
 * capabilities. WSL2 may expose no suitable Linux wireless interface.
 * Linux nl80211/radiotap UAPI headers are used directly; no libnl dependency.
 */

#ifndef PS_WIFI_SCANNER_H
#define PS_WIFI_SCANNER_H

#include "ps_scanner.h"

/* Wi-Fi operation mode. */
typedef enum ps_wifi_op_mode {
    PS_WIFI_MODE_MONITOR = 1  /**< rfmon mode — sees probe requests           */
} ps_wifi_op_mode_t;

/* Interface name length: POSIX IF_NAMESIZE is 16 (including NUL terminator).
 * We define our own constant to avoid depending on net/if.h include order.
 * The value 16 is stable across all Linux kernel versions. */
#define PS_IFACE_NAME_LEN  16

/* Configuration passed to ps_wifi_scanner_init(). */
typedef struct ps_wifi_scanner_config {
    char              iface[PS_IFACE_NAME_LEN]; /**< Empty selects a wireless interface */
    ps_wifi_op_mode_t op_mode;                  /**< Monitor capture only                 */
    int               scan_interval_s;          /**< Retained for config compatibility    */
} ps_wifi_scanner_config_t;

/**
 * @brief Initialise a Wi-Fi nl80211 scanner.
 *
 * Fills @p scanner with the nl80211 vtable and allocates private state.
 * Does NOT open the nl80211 socket yet — call ps_scanner_open() for that.
 *
 * @param scanner  Caller-allocated scanner instance.
 * @param config   Scanner parameters; NULL uses built-in defaults.
 * @return PS_OK or PS_ERR_INVALID_ARG / PS_ERR_ALLOC.
 */
ps_status_t ps_wifi_scanner_init(ps_scanner_t *scanner,
                                 const ps_wifi_scanner_config_t *config);

/**
 * @brief Return default Wi-Fi scanner configuration.
 */
ps_wifi_scanner_config_t ps_wifi_scanner_default_config(void);

#endif /* PS_WIFI_SCANNER_H */
