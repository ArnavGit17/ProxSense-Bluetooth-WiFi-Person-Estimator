# ProxSense — Validation Strategy

## Validation Status

Software-level tests cover the scanner parsers, Observation Manager, estimator math, and a simulation-to-manager-to-estimator path. These establish deterministic program behavior only; they do not validate Bluetooth/Wi-Fi radio reception, calibration, or person-count accuracy.

The current WSL2 workspace exposes no physical HCI controller or nl80211 wireless interface. End-to-end Bluetooth/Wi-Fi capture and person-count accuracy have not been hardware-validated. No accuracy results are claimed.

Phase 4B software verification on 2026-10-05: `make clean`, `make`, and `make tests` completed successfully; 7 test binaries passed, 0 failed, with 109 individual `PASS` cases. The clean build emitted no compiler warnings. These results do not establish RF capture or estimator accuracy.

## Deterministic Software Tests

Run the complete suite with:

```sh
make clean
make
make tests
```

Current test binaries:

| Test file | Coverage |
|---|---|
| `tests/test_bt_scanner.c` | Synthetic HCI parser behavior and observation-manager integration |
| `tests/test_wifi_scanner.c` | Synthetic radiotap/Probe Request parser safety and manager integration |
| `tests/test_obs_manager.c` | Filtering, deduplication, TTL, snapshot, and capacity behavior |
| `tests/test_observation.c` | Observation representation and enum helpers |
| `tests/test_sim_scanner.c` | Deterministic scenario playback and manager integration |
| `tests/test_estimator.c` | Estimator equations, health/zero semantics, calibration validation, persistence, coverage, rounding, uncertainty |
| `tests/test_estimator_integration.c` | Built-in simulation scanner → Observation Manager → snapshot → estimator |

Synthetic and simulation inputs must remain labelled as such. They are not live RF observations.

Estimator edge cases include empty/current snapshots, operational versus unknown/unavailable health, Bluetooth-only and Wi-Fi-only operation, both-radio overlap endpoints and intermediate values, separate source rates, persistence weights/thresholds, non-linear treatment of sighting count, unavailable RSSI, manager TTL expiry, random Bluetooth address diagnostics, invalid rates/overlap/coverage, explicit coverage correction, absent/configured uncertainty ranges, lower-bound clamping, and deterministic half-up rounding.

## Calibration and Live Validation Plan

Calibration requires native Linux hardware and controlled, person-labelled sessions. Record the people present, per-person device inventory and radio state, scanner operational health, per-source manager snapshot, effective record mass, separate source estimates, overlap, selected output target, uncertainty-range state, RSSI threshold, TTL, channel, adapter, driver, kernel, and session/environment identifiers.

Split training and evaluation by complete session and preferably by participant. Do not split adjacent time windows from one session across training and test data. Collect repeated trials for:

- empty room and feasible ground-truth populations such as 1, 2, 3, and 5 people;
- one versus multiple devices per person;
- Bluetooth-only, Wi-Fi-only, both-radio, and neither-radio conditions;
- participants with radios disabled or no supported device, if estimating all people via coverage $c$;
- controlled entry/exit, TTL delay, and static temporal stability;
- isolated known-device Bluetooth and Wi-Fi address rotation;
- different positions/orientations to characterize threshold sensitivity, not to infer distance;
- actual adapter monitor-mode capability, channel behavior, and packet capture health.

Fit $k_B$ and $k_W$ from labelled source records per represented person. Estimate $\rho$ only from paired per-person radio-visibility labels. Estimate coverage $c$ only when trials include people with no usable radio observation and the measured regime matches the intended deployment. Select persistence thresholds/weights only when held-out evaluation demonstrates benefit. Estimate signed residual quantiles on calibration sessions and evaluate range coverage on held-out sessions; do not label the range a confidence interval.

## Metrics

For known actual count $P_j$ and estimate $\widehat P_j$, record absolute error $|\widehat P_j-P_j|$. Aggregate MAE and RMSE:

$$
\mathrm{MAE}=\frac{1}{N}\sum_{j=1}^{N}|\widehat P_j-P_j|,\qquad
\mathrm{RMSE}=\sqrt{\frac{1}{N}\sum_{j=1}^{N}(\widehat P_j-P_j)^2}
$$

For $P_j>0$, percentage error is $100|\widehat P_j-P_j|/P_j$; omit it at zero and report false positives separately. Temporal evaluation should include mean absolute change between reports, variability during static ground truth, transition response delay, and oscillation count under a predeclared reversal rule. If uncertainty ranges are produced, report held-out empirical coverage and mean width. Any acceptance thresholds must be set as project goals before test runs and must not be represented as measured results until measured.

## Validation Boundaries

- **Software validation:** build/tests establish parser safety, estimator arithmetic, health/availability semantics, and deterministic integration behavior.
- **Hardware validation:** requires real Linux Bluetooth and Wi-Fi adapters; synthetic tests do not establish HCI reception, monitor-mode capture, or nl80211 driver behavior.
- **Person-count validation:** requires labelled people/device trials and held-out evaluation; no such accuracy result is currently available.
- WSL2 cannot prove over-the-air capture or that every nearby person/device will be observed.
