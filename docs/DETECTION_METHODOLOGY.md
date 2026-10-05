# ProxSense — Detection Methodology

## Why Raw Device Count ≠ Number of People

The most intuitive approach — "count visible devices, that is the number of people" — is incorrect for several compounding reasons:

| Problem | Effect on estimate |
|---|---|
| One person carries multiple devices | Device count > people count |
| Person has BT/Wi-Fi disabled | Device count < people count |
| BLE random address rotation | One device appears as many MACs |
| Infrastructure devices (APs, printers) | Non-person devices inflate count |
| RSSI variance causes re-appearing devices | Inflated transient count |

A valid person estimation system must model all of these effects explicitly.

---

## What Each Radio Technology Provides

### Bluetooth (BlueZ HCI passive scan)

**What we observe:**
- MAC address (6 bytes) — may be public (stable) or random (rotating)
- RSSI per advertising event (int8, dBm) — noisy signal measurement, not a person or distance measurement
- Address type — public vs. random LE
- Advertising data (manufacturer-specific, service UUIDs, TX power)
- Device name (if active scan requested or GAP name present in adv data)

**What this tells us:**
- A device address produced an advertising report received by the scanner
- RSSI is an optional, noisy observation value, not a person or distance measurement

**Key limitation — Resolvable Private Addresses (RPA):**
Modern smartphones (iOS since 2014, Android since 2015) use Bluetooth LE random addresses that rotate every ~15 minutes. Without the Identity Resolving Key (IRK, shared during Bluetooth pairing), these cannot be resolved to a stable identity. An unpaired phone will appear as a different device every rotation cycle. This is the primary challenge in BLE-based person counting.

**Current handling:** Random Bluetooth addresses are not resolved or grouped across rotations. The estimator reports their share as a diagnostic and lowers quality unless address-rotation behavior is included in calibration. It does not subtract a universal random-address discount.

---

### Wi-Fi (nl80211 monitor capture)

The Wi-Fi scanner uses nl80211 to inspect the selected Linux wireless interface
and requests a monitor interface when the selected interface is not already in
monitor mode. AF_PACKET receives radiotap-framed 802.11 traffic; only valid
Probe Request management frames currently become observations.

Managed-mode AP scans are deliberately not used. An access point is
infrastructure, not a client device and not evidence of a nearby person.
Client-side Probe Requests are more relevant because they are emitted by
devices searching for networks, but they are an incomplete signal: phones may
not probe while present, may suppress or randomize probes, and may have Wi-Fi
disabled or asleep.

For each accepted frame the scanner extracts the source MAC, frame type,
channel when a radiotap frequency field is present, and RSSI only when a valid
radiotap dBm signal field is present. A locally administered MAC bit is marked
as a privacy hint; it does not prove an address is random or stable. No
cross-radio identity is inferred, and a MAC address is not a person. A single
device can rotate addresses, while multiple devices can belong to one person.

Capture remains on the interface/PHY's current channel. This implementation
does not channel-hop; frames on other channels will be missed. Monitor support
depends on the adapter, driver, regulatory state, interface combinations, and
permissions. There is no managed-mode fallback. WSL2 in the current
development environment exposes no physical wireless interface, so over-the-
air capture has not been validated here.



---

## Implemented Estimator (Phase 4B)

The estimator operates on the manager's current `(source, MAC)` record snapshot, not raw device counts as people. It calculates effective record mass separately for Bluetooth and Wi-Fi. A transient-record weight is applied only from the configured persistence duration/sighting threshold; `sighting_count` is never multiplied into a people estimate. Defaults leave transient records at weight 1 and all estimator rates are explicitly placeholders until supplied and declared calibrated.

For each operational source, the source-represented estimate is effective record mass divided by that source's configured records-per-person parameter. When both live scanners are operational, the represented-population combination is `max(P_B, P_W) + (1 - rho) * min(P_B, P_W)`. The overlap term is an approximation calibrated from labelled trials, not a deterministic identity match. When one scanner is unavailable, only the other source's estimate is used. Neither enabled configuration nor equal MAC/name/RSSI/timing is treated as cross-radio identity or health evidence.

Live Bluetooth and Wi-Fi scanners are opened independently and feed the same Observation Manager. Successful open plus absence of fatal read errors defines operational status; ordinary timeouts and irrelevant/malformed packets do not count as hardware failure. Simulation health is separately labelled and never represents RF hardware health.

The default output is a radio-represented population estimate with LOW quality because calibration is unvalidated. All-person correction is disabled by default and is available only when explicitly enabled with a coverage value and calibration declaration, with both physical scanners operational. Coverage shift across environment/population invalidates that correction. An empirical uncertainty range is shown only when configured residual quantiles match a validated two-radio regime; it is an uncertainty range, not a confidence interval. No default calibration, accuracy, or person-count performance is claimed.

---

## Filtering Pipeline

The current Observation Manager applies the RSSI threshold, deduplication, and
TTL tracking. It does not implement Wi-Fi infrastructure filtering or a
minimum dwell-time filter. The Phase 4B estimator consumes the resulting
current per-source snapshot; it does not add an RSSI person multiplier.

### Filter 1: RSSI Threshold

Reject observations with RSSI below `rssi_min_dbm` (default: -85 dBm).

**Rationale:** Devices at -85 dBm are approximately 20–30 m away in open space. We are interested in people "nearby," not devices in adjacent rooms or on the street. This threshold is tunable via CLI.

**Edge case:** RSSI unavailable (`PS_RSSI_UNAVAILABLE`): the observation is accepted with a note that distance cannot be estimated. This prevents rejecting devices whose RSSI is simply not reported.

Infrastructure classification and minimum dwell-time filtering are not
implemented. Probe Request source addresses are not proof of a person or a
stable device identity.

---

## Deduplication

The Observation Manager maintains a table keyed by (source, MAC address). On repeated sighting:

- `last_seen` is updated
- `rssi_last_dbm` is updated
- `rssi_peak_dbm` is updated if RSSI is stronger
- `sighting_count` is incremented

This table is the deduplicated state. The estimator sees one record per unique MAC address.

---

## Temporal Tracking and Expiry

Devices not seen for `device_ttl_s` seconds (default: 60 s) are evicted from the table. This handles:

- People who have left the area
- Random-address rotation (the old random address expires, the new one creates a new entry)
- Transient infrastructure devices

The TTL is configured in the Observation Manager and defines the active record snapshot. It is deployment-dependent and should be evaluated for the intended reporting and exit latency; the estimator does not introduce a second TTL setting.
