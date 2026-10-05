# ProxSense — System Architecture

## Overview

ProxSense is structured as a pipeline of independent modules. Each module has a single responsibility and communicates with adjacent modules through well-defined interfaces (C function calls or data structs). There is no shared global state between modules.

```
┌─────────────────────────────────────────────────────────────┐
│                      CLI / main.c                           │
│   (argument parsing, event loop, output, signal handling)   │
└───────────────────────┬─────────────────────────────────────┘
                        │ ps_config_t
                        ▼
        ┌───────────────────────────────┐
        │      Scanner Abstraction      │
        │   ps_scanner_t  (vtable)      │
        │   ps_scanner_ops_t interface  │
        └───┬──────────────┬────────────┘
            │              │
  ┌─────────▼──────┐  ┌────▼──────────────┐  ┌──────────────────┐
    │  BT Scanner    │  │  Wi-Fi Scanner    │  │  Sim Scanner     │
    │  (BlueZ HCI)   │  │ (nl80211 + packet)│  │  (file replay)   │
    │  Phase 2       │  │  Phase 3          │  │  Simulation      │
  └─────────┬──────┘  └────┬──────────────┘  └──────┬───────────┘
            │              │                          │
            └──────────────┼──────────────────────────┘
                           │ ps_observation_t (per event)
                           ▼
            ┌──────────────────────────────┐
            │     Observation Manager      │
            │   ps_obs_manager_t           │
            │                              │
            │  1. Filter (RSSI, TTL)       │
            │  2. Deduplicate (MAC table)  │
            │  3. Temporal tracking        │
            │  4. Expiry (LRU eviction)    │
            └──────────────┬───────────────┘
                           │ ps_device_record_t[] snapshot
                           ▼
            ┌──────────────────────────────┐
            │   Phase 4B Estimator        │
            │   ps_estimator_t             │
            │                              │
            │  Per-radio rates + overlap   │
            │  Optional coverage/range     │
            └──────────────┬───────────────┘
                           │ ps_estimate_t
                           ▼
            ┌──────────────────────────────┐
            │    Output / CLI Layer        │
            │    (main.c report loop)      │
            └──────────────────────────────┘
```

---

## Module Descriptions

### 1. CLI / Application Layer (`main.c`, `ps_config.h`)

**Purpose:** Owns `main()`. Parses CLI arguments, instantiates and wires together all other modules, runs the event loop, handles signals.

**Inputs:** `argc`, `argv`
**Outputs:** Console output (estimates, logs)
**Dependencies:** All other modules
**Failure cases:**
- Invalid CLI arguments → print usage, `EXIT_FAILURE`
- No scanner available and mode is `--live` → warn and exit
- Signal (SIGINT/SIGTERM) → clean shutdown

---

### 2. Scanner Abstraction (`ps_scanner.h`)

**Purpose:** Defines the `ps_scanner_ops_t` vtable interface that all scanner back-ends implement. This is the only API the Observation Manager knows about.

**Why a vtable?** It allows scanner back-ends to be swapped or combined at runtime without changing the pipeline. The estimator never sees BlueZ or nl80211.

**Inputs:** Nothing (interface definition)
**Outputs:** `ps_observation_t` per event (via `read_next`)

---

### 3. Bluetooth Scanner (`ps_bt_scanner.h / .c`)

**Purpose:** Implements `ps_scanner_ops_t` using a raw HCI socket (`AF_BLUETOOTH / BTPROTO_HCI`).

**Technology:** BlueZ — specifically the raw HCI socket API from `<bluetooth/hci.h>`.

**Why raw HCI instead of D-Bus?**
- D-Bus BlueZ API exposes paired/connected devices only; it does not give per-event RSSI for passive advertising scans.
- The HCI socket gives direct access to every LE advertising event, including RSSI, address type, and advertising data payload.
- HCI sockets are used internally by `btmon`, `hcitool`, and `bluetoothd` itself.

**Inputs:** `ps_bt_scanner_config_t` (HCI device index, scan mode, interval/window)
**Outputs:** `ps_observation_t` with source=BLUETOOTH, MAC, RSSI, address type
**Dependencies:** `libbluetooth` (package `libbluetooth-dev`)
**Failure cases:**
- No BT adapter → `PS_ERR_NO_HARDWARE`
- No `CAP_NET_RAW` → `PS_ERR_PERMISSION`
- HCI socket failure → `PS_ERR_IO`
- WSL2 (no controller) → `PS_ERR_NO_HARDWARE`

---

### 4. Wi-Fi Scanner (`ps_wifi_scanner.h / .c`)

**Purpose:** Implements `ps_scanner_ops_t` using nl80211 generic-netlink
messages and an AF_PACKET monitor capture socket. The Linux UAPI is used
directly; libnl is not a build dependency.

**Capture behavior:** An explicit interface may be selected with
`--wifi-interface`; otherwise the scanner selects a wireless interface exposed
through `/sys/class/net`. If the selected interface is not already in monitor
mode, the scanner requests a virtual monitor interface and removes it during
close. Captured radiotap packets are bounds-checked and only valid Probe Request
frames are converted to Wi-Fi observations. Capture remains on the PHY's
current channel; there is no channel hopping or managed-mode AP fallback.

**Outputs:** `ps_observation_t` with source=WIFI, transmitter MAC, optional
radiotap RSSI/channel, and a locally-administered-address hint.
**Dependencies:** Linux nl80211/radiotap UAPI headers; no libnl library.
**Failure cases:** missing interface → `PS_ERR_NO_HARDWARE`; unsupported monitor
interface/driver → `PS_ERR_UNSUPPORTED`; permission failure →
`PS_ERR_PERMISSION`; socket/netlink failure → `PS_ERR_IO`.

---

### 5. Simulation Scanner (`ps_sim_scanner.h / .c`)

**Purpose:** Replays a pre-written scenario file as `ps_observation_t` events. Used for all automated testing and WSL2 development.

**Critical rule:** Simulation mode is mutually exclusive with live mode. The CLI enforces this. All simulation output is clearly labelled `[SIMULATION]` in logs.

**Inputs:** Scenario file path, realtime flag
**Outputs:** `ps_observation_t` per scenario line
**Failure cases:** File not found → `PS_ERR_IO`; malformed line → `PS_ERR_MALFORMED`

---

### 6. Observation Manager (`ps_obs_manager.h / .c`)

**Purpose:** Receives raw observations from scanners and maintains a live table of currently-visible devices after filtering, deduplication, and expiry.

**Sub-responsibilities:**
- **Filter:** Reject observations below the configured RSSI threshold. Infrastructure and dwell-time filters are not implemented.
- **Deduplicate:** Maintain a flat hash/linear table keyed by MAC address.
- **Track:** Update `first_seen`, `last_seen`, `sighting_count`, `rssi_last`, `rssi_peak`.
- **Expire:** Remove devices not seen within `device_ttl_s` seconds.

**Inputs:** `ps_observation_t` (from scanners), `ps_filter_config_t`
**Outputs:** `ps_device_record_t[]` snapshot (to estimator)
**Failure cases:** Table overflow → evict LRU entry, log warning; null pointer → `PS_ERR_INVALID_ARG`

---

### 7. Estimator (`ps_estimator.h / .c`)

**Purpose:** Converts active per-source device records into separate Bluetooth
and Wi-Fi represented-population estimates using explicit records-per-person
parameters, then applies a calibrated overlap approximation. Optional
all-person coverage correction and held-out residual uncertainty ranges are
disabled unless explicitly configured.

**Health contract:** The main loop reports scanner operational health from
successful open and absence of fatal read errors, not merely enabled
configuration. Both live scanners are polled into the same manager. Simulation
health is labelled synthetic and is not RF hardware validation.

**Calibration status:** Built-in parameters are placeholders. A numeric result
using those defaults is LOW quality and unvalidated; device counts are not
people counts. Output distinguishes radio-represented and all-person targets,
and uses an uncertainty range rather than calling it a confidence interval.

---

### 8. Logging (`ps_log.h / .c`)

**Purpose:** Provides timestamp-prefixed, level-filtered logging to stderr.

**Properties:** No external dependencies; compile-time level filtering; thread-safe for simple use.

---

## Data Flow Summary

```
Bluetooth Scanner ─┐
                  ├──> ps_observation_t ──> Observation Manager
Wi-Fi Scanner ────┘
```

```
Hardware Events
    │
    ▼ (HCI advertising events / radiotap 802.11 frames)
Scanner Back-end
    │
    ▼ ps_observation_t  (one struct per detection event)
Observation Manager
    │
    ▼ ps_device_record_t[]  (deduplicated, RSSI-filtered, TTL-valid snapshot)
Estimator
    │
    ▼ ps_estimate_t  (represented/all target, availability, quality, optional uncertainty range)
CLI Output
```

---

## Design Decisions

### Why C11?
C11 provides `_Static_assert`, `_Alignas`, anonymous unions, and `<stdint.h>` without external dependencies. It is universally supported on GCC 13 / Ubuntu 24.04. C11 does not mandate a threading model, keeping the design simple for Phase 0.

### Why no global state?
Every module takes its configuration and state as an explicit pointer argument. This makes testing trivial (instantiate a second copy for a test), avoids order-of-initialisation bugs, and makes the ownership of each resource obvious.

### Why a flat device table instead of a hash map?
With `PS_OBS_MANAGER_MAX_DEVICES = 512`, a linear scan is O(n) but n is small. A flat table avoids the complexity of a hash map implementation and eliminates pointer-chasing. If profiling in Phase 2 shows this is a bottleneck, a hash table can replace it behind the same interface without touching any other module.

### Why vtable (ops pointer) for scanners?
The vtable pattern is the standard C idiom for polymorphism without C++. It means:
- `ps_obs_manager_process()` does not need `#ifdef HAVE_BLUETOOTH` guards.
- Unit tests can inject a stub scanner without recompiling the pipeline.
- A future BLE-only build can simply omit the Wi-Fi back-end object file.
