/**
 * @file ps_bt_scanner.h
 * @brief Bluetooth scanner back-end interface (BlueZ / HCI).
 *
 * Technology choice rationale:
 *
 *   BlueZ is the official Linux Bluetooth stack and is the only stack shipped
 *   by default in all major distributions.  It provides two practical paths:
 *
 *   Path A — Raw HCI socket (AF_BLUETOOTH / BTPROTO_HCI):
 *     - Directly reads HCI event packets from the kernel driver.
 *     - Provides full LE advertising data, RSSI, address type, and PDU type.
 *     - Requires CAP_NET_RAW capability (or running as root).
 *     - Avoids D-Bus and the BlueZ daemon overhead entirely.
 *     - Used by hcitool, btmon, and hcidump internally.
 *     - Well-documented in <bluetooth/bluetooth.h> and <bluetooth/hci.h>.
 *     - Chosen for Phase 1 because it gives direct, unmediated access to the
 *       information we need (RSSI, address type, advertising PDU) without
 *       going through D-Bus object model complexity.
 *
 *   Path B — D-Bus / BlueZ GATT object model:
 *     - High-level but hides raw advertising RSSI per event.
 *     - Better for pairing/connection but overkill for passive scanning.
 *     - Would require libdbus-1 or GLib/GIO dependency.
 *     - Deferred to a future optional back-end if needed.
 *
 *   Information available via HCI socket:
 *     - MAC address (6 bytes)
 *     - Address type: public (permanent) or random (may rotate)
 *     - RSSI: yes, per advertising event (-127 to +20 dBm)
 *     - Device name: only if device responds to name request (not in passive scan)
 *     - PDU type: ADV_IND, ADV_NONCONN_IND, SCAN_RSP, etc.
 *     - Advertising data payload (manufacturer data, service UUIDs, etc.)
 *
 *   Important limitation — Random (Resolvable) Private Addresses (RPA):
 *     Modern BLE devices (especially Apple and Android phones) rotate their
 *     MAC address every ~15 minutes using Resolvable Private Addresses.
 *     Without the peer's IRK (Identity Resolving Key, exchanged after pairing)
 *     these cannot be resolved to a stable identity.
 *     This is the central challenge in BLE-based person counting and must be
 *     handled explicitly in the deduplication and estimation layers.
 *
 * WSL2 note:
 *   WSL2 does not expose the host Bluetooth controller.  This module cannot
 *   open an HCI socket in a WSL2 environment.  The simulation back-end in
 *   ps_sim_scanner.h is used for development and testing instead.
 */

#ifndef PS_BT_SCANNER_H
#define PS_BT_SCANNER_H

#include "ps_scanner.h"

/* HCI device index (0 = hci0, 1 = hci1, …).
 * PS_BT_SCANNER_DEFAULT_HCI selects whichever controller the kernel reports
 * first.  Override via CLI --bt-hci flag. */
#define PS_BT_SCANNER_DEFAULT_HCI  -1

/* Passive scan: listen for advertising packets without sending scan requests.
 * Active scan: send SCAN_REQ to get SCAN_RSP (name, more data) but reveals
 *              our own MAC to the scanned device.
 * We default to passive scan for privacy reasons. */
typedef enum ps_bt_scan_mode {
    PS_BT_SCAN_PASSIVE = 0,
    PS_BT_SCAN_ACTIVE  = 1
} ps_bt_scan_mode_t;

/* Configuration passed to ps_bt_scanner_init(). */
typedef struct ps_bt_scanner_config {
    int               hci_dev_id; /**< HCI device index, e.g. 0 for hci0     */
    ps_bt_scan_mode_t scan_mode;  /**< Passive (default) or active            */
    uint16_t          scan_interval_slots; /**< LE scan interval in 0.625 ms units */
    uint16_t          scan_window_slots;   /**< LE scan window in 0.625 ms units   */
} ps_bt_scanner_config_t;

/**
 * @brief Initialise a Bluetooth HCI scanner.
 *
 * Fills @p scanner with the BlueZ HCI vtable and allocates private state.
 * Does NOT open the HCI socket yet — call ps_scanner_open() for that.
 *
 * @param scanner  Caller-allocated scanner instance to initialise.
 * @param config   Scanner parameters; NULL uses built-in defaults.
 * @return PS_OK or PS_ERR_INVALID_ARG / PS_ERR_ALLOC.
 */
ps_status_t ps_bt_scanner_init(ps_scanner_t *scanner,
                               const ps_bt_scanner_config_t *config);

/**
 * @brief Return default Bluetooth scanner configuration.
 * Callers who only want to override one or two fields should start here.
 */
ps_bt_scanner_config_t ps_bt_scanner_default_config(void);

#endif /* PS_BT_SCANNER_H */
