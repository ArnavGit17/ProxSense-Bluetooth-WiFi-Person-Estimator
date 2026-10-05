# ProxSense — Assumptions and Limitations

This document is a first-class engineering artifact, not a disclaimer buried in an appendix. Understanding these limitations is essential for interpreting any output from ProxSense.

---

## Development Environment Assumptions

### WSL2 Hardware Access

**Assumption:** The project will eventually be tested and deployed on native Linux with physical Bluetooth and Wi-Fi hardware.

**Current reality:** The current WSL2 environment exposes no physical Bluetooth HCI
controller or Linux wireless interface suitable for nl80211 monitor capture.
This is an environment observation, not a guarantee about every WSL setup.
The Wi-Fi scanner detects interface/capability failures and does not substitute
simulated results in live mode.

**Mitigation:** Parser and observation-pipeline tests use synthetic packet
fixtures. End-to-end over-the-air Wi-Fi capture requires native Linux with a
compatible adapter and driver; that validation has not been performed here.

---

## Fundamental Estimation Limitations

### 1. People Without Detectable Devices

**Limitation:** ProxSense cannot detect people whose Bluetooth and Wi-Fi are both disabled, or who carry no wireless devices.

**Impact:** Systematic undercount in any environment where some fraction of people choose not to use wireless devices (elderly users, children, people in certain work environments).

**No mitigation available within the current design.** This is an inherent limitation of RF-based counting.

### 2. Bluetooth Random Address Rotation

**Limitation:** Modern smartphones (iOS since 2014, Android since approximately Android 8 for Wi-Fi, Android 10 for BLE) randomise their MAC addresses on a per-session or timed basis.

**Impact:**
- A single physical device may appear as multiple unique MAC addresses over a session, inflating device count.
- The same physical device cannot be reliably tracked across address rotation events without the device's Identity Resolving Key (IRK), which is only available after Bluetooth pairing.
- The exact rotation interval and policy varies by OS, manufacturer, and Android version.

**Current handling:** The estimator does not resolve or deterministically correlate rotated addresses. A Bluetooth random-address fraction is reported as a diagnostic and can lower quality unless rotation behavior is included in deployment calibration. Wi-Fi's locally-administered hint is not retained in manager records, so Wi-Fi rotation cannot currently be classified by the estimator.

### 3. Multi-Device Persons

**Assumption (to be calibrated):** On average, a person in this environment carries approximately N detectable wireless devices.

**Limitation:** N is environment-dependent. In a consumer setting it may be 2–3 (phone, watch, earbuds). In an enterprise setting it may be 1 (corporate laptop with BT/Wi-Fi controlled). In a general public setting the distribution is wide.

**Impact:** Underestimating N overestimates people count; overestimating N underestimates it.

**Mitigation:** The per-radio records-per-person rates and overlap parameter in `ps_estimator_config_t` require labelled validation experiments in the target environment. Built-in values are explicit placeholders; a result using them is marked LOW and unvalidated. An empirical uncertainty range is shown only when matching residual quantiles are supplied.

### 4. Infrastructure Devices

**Limitation:** The environment may contain Bluetooth speakers, smart TVs, building automation controllers, wireless printers, and other fixed devices that are not people.

**Impact:** These inflate the device count if not filtered.

**Current handling:** No general infrastructure-device filter is implemented in the Observation Manager. The Wi-Fi scanner currently emits only valid Probe Request observations, but those are still not proof of a person or a stable device.

---

## Technical Limitations

### 5. RSSI Is Not Distance

**Estimator behavior:** RSSI is not converted to distance or used as a person multiplier. The Observation Manager applies its configured RSSI threshold; the estimator accepts unavailable RSSI according to manager behavior.

**Limitation:** RSSI is affected by:
- Multipath fading (reflections from walls, furniture)
- Body absorption (a phone in a pocket vs. on a desk)
- Antenna orientation and polarisation
- Interference from other 2.4 GHz devices
- TX power level differences between devices

**Impact:** The manager's threshold is only a rough upstream observation filter, not a precise distance boundary. The estimator does not infer distance or population from RSSI.

### 6. Scan Timing

**Limitation:** Bluetooth LE advertising and Wi-Fi probe requests are asynchronous. A device may not be observed in every scan interval even if the person is present.

**Impact:** Intermittent observations cause devices to appear and disappear in the table even without actual movement. The TTL expiry mitigates stale records; no minimum dwell-time filter is currently implemented.

### 7. Wi-Fi Monitor Capture Limitations

**Monitor mode dependency:** Client Probe Request capture requires monitor-mode
support from the specific adapter and driver. nl80211 interface creation may be
rejected even when the interface exists. The scanner reports that failure and
never falls back to managed-mode AP scanning. Enabling monitor mode may require
`CAP_NET_ADMIN`; opening the raw packet socket requires `CAP_NET_RAW`.

**Probe coverage:** Probe Requests are useful client-side activity, not a
complete inventory. A device may not emit them while nearby because of
power-saving, operating-system privacy behavior, connection state, or disabled
Wi-Fi. Not every phone is discoverable and no person's presence is guaranteed
to produce an observation.

**Address privacy:** Wi-Fi clients may use locally administered/private MACs,
rotate temporary addresses, or change address policy across networks and
sessions. The locally administered bit is only a hint, not proof of
randomization or stable identity. Different Wi-Fi MACs do not imply different
people, and Wi-Fi addresses cannot reliably be correlated with Bluetooth
addresses in this phase.

**RSSI and channel:** RSSI is used only when the capture driver provides a
valid radiotap dBm signal field; otherwise it is marked unavailable. Even
reported RSSI is affected by antenna orientation, body absorption, multipath,
and transmit power. Capture follows the monitor interface's current PHY
channel; the implementation does not hop channels, so traffic elsewhere is
missed. It does not change the selected channel.

### 8. Regulatory Domain

**Limitation:** Some Wi-Fi channels (12, 13, 14 in the 2.4 GHz band; portions of 5 GHz) are restricted by regulatory domain. Devices operating on restricted channels may not be seen.

---

## Calibration Assumptions

### 9. Estimation Parameters Are Uncalibrated by Default

**Assumption:** The built-in Bluetooth/Wi-Fi records-per-person and overlap values in `ps_estimator_config_default()` are placeholders, not real measurements. Coverage and residual-range corrections are disabled by default; persistence settings default to unit record weight.

**Impact:** Estimates using placeholder values are LOW quality and unvalidated. They must not be presented as measured person counts.

**What must be done:** Run the validation experiments described in `docs/VALIDATION.md` with known ground-truth people and device inventories; fit parameters and evaluate residuals on held-out sessions.

The optional all-people correction uses $c$, the calibrated fraction of people represented by at least one operational radio in the defined window. It is disabled by default, requires both physical scanners operational, and is valid only for the population/environment regime in which it was measured. Environmental, population, device-mix, adapter, or channel shifts can invalidate this correction; no all-person estimate should be claimed outside supported calibration.

### 10. Single Environment Calibration

**Limitation:** Model parameters calibrated in one environment (e.g., office room) may not generalise to different environments (open plan office, corridor, outdoor space).

**Impact:** Accuracy may degrade when the system is moved to a different physical environment.

---

## Privacy and Legal Limitations

### 11. Passive Radio Scanning

**Important notice:** ProxSense passively reads radio signals that are broadcast by devices. In most jurisdictions, passively receiving broadcast radio signals is legal. However:

- The legal status of logging MAC addresses may vary by jurisdiction.
- Organisations deploying this system should consult their legal/privacy teams before deployment.
- ProxSense in its current design does NOT store MAC addresses beyond the current session (no persistent logging of individual device identities is implemented).

This document does not constitute legal advice.

---

## Summary Table

| Limitation | Severity | Mitigation Available | Phase |
|---|---|---|---|
| WSL2 no hardware access | Development blocker | Sim scanner | 0 |
| Wi-Fi monitor capability varies by adapter/driver | High | Detect and report unsupported capability | 3 |
| Probe Requests are intermittent/incomplete | High | No complete detection guarantee | 3 |
| Wi-Fi MAC randomization and rotation | High | No stable identity claim | 3 |
| Wi-Fi monitor capture is current-channel only | High | User/OS controls channel | 3 |
| Undetectable people (no device) | Fundamental | None | — |
| BLE random address rotation | High | Diagnostic and deployment-specific uncertainty calibration; no address resolution | 4B |
| Multi-device persons | High | Calibrated per-radio records/person rates and overlap | 4B |
| Infrastructure device inflation | Medium | No general manager-side infrastructure filter | 4B |
| RSSI ≠ distance | Medium | Tunable threshold | 1 |
| Uncalibrated estimator parameters | High until calibrated | Labelled training sessions and held-out evaluation | 4B |
| Environment-specific calibration | Medium | Per-deployment config | 2 |
