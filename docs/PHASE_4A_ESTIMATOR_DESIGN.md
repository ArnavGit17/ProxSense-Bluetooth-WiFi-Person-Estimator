# Phase 4A — Person Estimator Architecture and Design

**Status: approved design record.** Phase 4B now implements this model. Its built-in parameters remain uncalibrated placeholders; no WSL2 RF hardware validation or person-count accuracy result is claimed.

## Existing Data Available

The Observation Manager currently exposes a snapshot containing one `ps_device_record_t` per `(source, MAC)` key that survived its RSSI threshold and TTL. Each record has:

- source (`PS_SOURCE_BLUETOOTH` or `PS_SOURCE_WIFI`) and normalized MAC text;
- manager-assigned monotonic `first_seen` and `last_seen`;
- aggregate `sighting_count`;
- last and peak RSSI (`PS_RSSI_UNAVAILABLE` when not supplied);
- `is_random_bt`, set for Bluetooth random addresses.

On repeats the manager updates `last_seen`, increments the count and updates RSSI. TTL expiry removes stale entries. Snapshot records are current state, not event history.

The current schema does **not** provide event timestamps/inter-arrival gaps, Wi-Fi's locally-administered bit, full Bluetooth address metadata, a ground-truth label, or a cross-radio identity. It has no AP/client classification table. A Wi-Fi Probe Request's locally-administered bit currently does not survive into `ps_device_record_t`. The manager's `min_duration_s` and `filter_random_bt` fields are not applied in the implementation; current behavior is RSSI filtering, deduplication, TTL and capacity eviction. An estimator must not assume otherwise.

At the Phase 4A review, `main.c` derived scanner availability from enabled configuration and selected one live scanner, so both radios did not feed the same Observation Manager. Phase 4B subsequently changed the live path to open enabled scanners independently and poll both into the same manager. Its per-source operational status is based on successful open, polling during the reporting window, and no fatal read error; enabled configuration alone is not treated as operational health. This confirms the software control path only: simultaneous operation/coexistence on native Linux radio hardware remains unvalidated because the current WSL2 environment exposes no physical HCI or nl80211 interface.

## Estimation Problem

At report time $t$, the input is the current non-expired manager snapshot $R_t$, actual per-radio operational status for the reporting window, and a versioned calibration. The desired output is an estimate of people present in a defined area. Distinguish two targets:

1. **Radio-represented population:** people with at least one usable Bluetooth or Wi-Fi observation in the defined window.
2. **All people present:** includes people with no detectable radio signal.

The snapshot can support a calibrated estimate of the first target. Estimating the second also needs an empirically measured coverage probability for people represented by either radio. Without it, report the radio-represented quantity or “insufficient evidence”; never silently assume everyone is detectable.

## Candidate Models

**A. Pooled device/person ratio.** Divide a pooled device count by one devices-per-person constant. It is simple, but hides differences between Bluetooth and Wi-Fi visibility and mishandles duplicate people across radios. Poor fit to this project's inputs.

**B. Separate source rates plus calibrated overlap.** Estimate a Bluetooth-represented population and a Wi-Fi-represented population separately, then subtract a calibrated intersection. It is transparent set arithmetic, needs paired labelled trials, and is explainable with the current per-source snapshot. **Recommended for a first implementation.**

**C. Regression or Bayesian count model.** Fit count distributions using source, persistence, RSSI and other features. Such models can represent dispersion and uncertainty, but current records discard event-level history and Wi-Fi randomization metadata. They require more data and assumptions, increasing overfit and false-precision risk. Defer until richer data and sufficient labelled trials exist.

| Model | Simplicity | Calibration | Fit to current inputs | Explainability | Overfit risk |
|---|---|---|---|---|---|
| A: pooled ratio | High | One pooled rate | Low | Medium | Medium |
| B: separate rates + overlap | Medium | Two rates and overlap; optional coverage | Best | High | Manageable with held-out sessions |
| C: regression/Bayesian | Low-medium | Large labelled dataset and model checks | Low without richer history | Medium-low | High initially |

## Recommended Model and Equations

Version 1 should estimate radio-represented populations first. Let $s\in\{B,W\}$ denote Bluetooth or Wi-Fi and $R_{s,t}$ be active records for that source. For record $i$, let $a_i=t-\text{last\_seen}_i$, $d_i=\text{last\_seen}_i-\text{first\_seen}_i$, and $n_i$ be `sighting_count`. These times use the manager's monotonic clock. Define a bounded evidence weight:

$$
w_i=I(a_i<T_s)g_s(d_i,n_i),\qquad D_{s,t}=\sum_{i\in R_{s,t}}w_i
$$

The manager snapshot already enforces its TTL, so $I(a_i<T_s)$ is a consistency check only. $T_s$ comes from the manager's TTL; it is not an independently configured estimator policy. A simple candidate persistence rule is:

$$
g_s(d,n)=\begin{cases}
q_s,&d<D_{s,\min}\;\text{or}\;n<n_{s,\min}\\
1,&\text{otherwise}
\end{cases}\qquad 0\le q_s\le1
$$

$D_{s,t}$ is effective record mass, not a number of people. $q_s$, $D_{s,\min}$ and $n_{s,\min}$ must be fitted or selected using labelled training sessions; use a `sighting_count` persistence feature or threshold only if held-out validation demonstrates benefit. `sighting_count` must never be interpreted as proportional evidence of the number of people: repeated sightings and persistence can only temper record evidence, not count additional people. It has no event times and different devices produce observations at different rates, so do not use it as a linear multiplier.

Let $k_s$ be the calibrated effective records per person represented by source $s$. Then:

$$
\widehat P_{B,t}=D_{B,t}/k_B,\qquad \widehat P_{W,t}=D_{W,t}/k_W
$$

These are source-represented population estimates, not all-person counts. $k_B$ and $k_W$ are deployment-specific parameters. Existing defaults (`1.5`, `1.0`, `0.5`, `0.5`, `5`) are uncalibrated placeholders and are not evidence or recommendations.

## Cross-Radio Overlap

Do not add raw Bluetooth and Wi-Fi device counts. One person can generate several records within a radio and can be represented in both. Dividing their sum by a single devices-per-person value still double counts people seen by both radios, assumes device ratios are identical, and cannot account for people represented by only one source.

Define $\rho$ as the expected fraction of the smaller **represented-person population** that is also represented by the other radio in the same reporting window:

$$
\rho=\frac{|P_B\cap P_W|}{\min(|P_B|,|P_W|)},\qquad 0\le\rho\le1
$$

$P_B$ and $P_W$ here are person sets known only in controlled calibration, not runtime MAC sets. Estimate $\rho$ from per-person visibility labels in simultaneous paired-radio trials. It is neither an address-match probability nor a parameter applied to raw device counts.

For jointly available sources, combine the separate estimates as:

$$
\widehat P_{\mathrm{any},t}=\max(\widehat P_{B,t},\widehat P_{W,t})+(1-\rho)\min(\widehat P_{B,t},\widehat P_{W,t})
$$

Equivalently, sum the two source populations and subtract $\rho$ times the smaller estimate. $\rho=0$ gives a sum (no overlap); $\rho=1$ gives the larger source estimate (the smaller is contained). These endpoint meanings help make the parameter auditable. For one available radio, use only its estimate and mark degraded quality; do not apply overlap correction. Never match BT and Wi-Fi identities by equal MAC text, name, RSSI, or timing.

The term $\rho\min(\widehat P_{B,t},\widehat P_{W,t})$ is a calibrated approximation to the expected represented-person intersection, not a mathematical identity. Its validity must be evaluated on held-out sessions; if it does not generalize to the deployment regime, the two-radio combination is unsupported.

For an all-people estimate, define optional $c$ as the fraction of people present represented by at least one usable radio during the defined window:

$$
c=P(\text{at least one usable BT or Wi-Fi record}\mid\text{person present})
$$

Only with supported paired trials may the estimate be expanded as $\widehat P_{\mathrm{all}}=\widehat P_{\mathrm{any}}/c$. This assumes the calibrated coverage probability $c$ applies to the deployment regime and current population, device mix, and operating conditions; environmental or population shift can invalidate the correction. Include uncertainty in $c$. If $c$ is not measured for, or does not apply to, the deployment regime, do not claim an all-person estimate.

## Temporal Evidence and RSSI

The current snapshot provides age, total persistence span and aggregate sighting count, but not an event sequence. TTL removes stale entries; expiry is a policy boundary, not proof of the moment a person left. Version 1 should recompute from each current snapshot and avoid long smoothing. Initially omit hysteresis; if held-out temporal tests demonstrate count jitter, add bounded display-only hysteresis with calibrated response delay, while allowing sustained arrivals/departures through promptly. Do not keep records beyond manager TTL.

Use RSSI only through the existing manager threshold and as a reported quality diagnostic. Do not make stronger RSSI mean “one closer person” or use it as a person multiplier. RSSI is noisy, unavailable for some records, and already filtered upstream; estimator weighting would double-apply the threshold without a validated model. Record the threshold in calibration/evaluation metadata.

## Randomized Addresses

The manager keys by `(source, MAC)`, so address rotation may inflate active record counts. The Bluetooth-random flag identifies an address category but cannot resolve RPA or connect rotated addresses. Wi-Fi's local-admin bit is not retained in the manager record, so the estimator cannot even stratify likely randomized Wi-Fi addresses at present. Do not infer stable identity, perform deterministic clustering, or correlate across radios.

Version 1 should expose Bluetooth-random share as a diagnostic/quality input. Rotation baselines using known single devices may inform a source-specific empirical uncertainty allowance, not a universal fixed subtraction. If rotation is uncalibrated or outside supported conditions, retain the observable-record point estimate only with wider uncertainty or low quality. State Wi-Fi identity uncertainty as unobserved by the current estimator input.

## Uncertainty and Data Quality

Use **uncertainty range**, not “confidence interval,” unless a statistical procedure and assumptions later justify that terminology. One candidate is empirical held-out residual quantiles from independent sessions. For training residuals define $e_j=\widehat P_j-P_j$ and estimate lower/upper quantiles $q_L,q_U$ for the chosen, predeclared coverage level. Then the corresponding prediction range is:

$$
[L,U]=[\max(0,\lfloor\widehat P-q_U\rfloor),\;\max(0,\lceil\widehat P-q_L\rceil)]
$$

The signs reverse because actual count equals estimate minus residual. Evaluate coverage and range width on sessions/participants excluded from fitting. This empirical range is not a formal confidence interval and does not guarantee transfer to another environment. With sparse, shifted, or out-of-regime calibration, provide no numeric range and return insufficient evidence.

Quality should express evidence support, not an arbitrary probability:

- **High:** both radios confirmed operational over the window; evidence and random-address behavior fall within validated regimes; held-out range is available for that regime.
- **Medium:** one radio only, limited but supported evidence, or a wider supported range.
- **Low / insufficient:** scanner health unknown, neither source operational, inputs out of calibration support, or no defensible range. The current `PS_QUALITY_GOOD/DEGRADED/UNAVAILABLE` enum is too coarse; a future output should separately flag estimate and range availability.

Distinguish no records from zero people. A numeric zero represented-population estimate is only plausible after both relevant sources were confirmed operational for a validated window. Even then, all-person count needs calibrated coverage $c$. If scanner availability is unknown, return insufficient evidence, not zero.

## Calibration Parameters

Calibration values are parameters to be measured, not facts. $T_s$ is the manager-owned TTL setting to verify, not an independent estimator calibration value. Record calibration version, date, area, participant/device mix, environment, scanner settings, TTL/RSSI threshold, adapter/driver/kernel, channel behavior, and train/test session identifiers.

| Parameter | Meaning | Experiment to estimate | Environment dependence |
|---|---|---|---|
| $k_B$ | Effective active BT records per BT-represented person after persistence rule | Known participants and per-person device inventory; BT-only and mixed trials | High: device habits, population, placement, window |
| $k_W$ | Effective active Probe Request records per Wi-Fi-represented person | Wi-Fi-only and mixed trials with device inventory; repeat across people, time and channels | Very high: OS, association/power state, privacy, channel, adapter/driver |
| $\rho$ | Fraction of smaller represented-person population also represented by the other radio | Simultaneous per-person BT/Wi-Fi visibility labels; intersection divided by smaller represented population per session | High: radio state, user mix, scan conditions |
| $c$ (optional) | Fraction of all present people represented by either radio | Include participants with radios disabled/no supported device; repeat in each supported context | Very high; required for all-person estimate |
| $q_s,D_{s,\min},n_{s,\min}$ | Persistence downweight and its duration/count boundary | Label short pass-through versus sustained presence, fit on training sessions, compare held-out error | Scan cadence/device behavior dependent |
| $T_s$ (manager setting) | Manager-owned active snapshot horizon, read by the estimator only; not an independent estimator setting | Verify manager expiry behavior with controlled exits and background scans; measure exit latency and stale retention | Arrival/departure pattern and scan cadence |
| $q_L,q_U$ | Held-out residual quantiles for a declared range level | Fit on training sessions; evaluate coverage on independent sessions/people | Strongly deployment/regime dependent |
| Rotation allowance | Empirical excess unique addresses per known physical device, used only for uncertainty | Isolated single-device BT and Wi-Fi rotation baselines | OS/device/privacy setting/window dependent |

Do not assign universal real-world values. If a parameter is unidentifiable, restrict the supported operating regime or return insufficient evidence.

## Validation Plan and Metrics

Use native Linux hardware for live trials; synthetic tests validate parser logic only. Repeat trials across sessions/days and separate training and evaluation by complete session and preferably participant, not adjacent samples from one session. Do not claim results until measured.

For every reporting window log ground-truth people, each participant's radio state, per-source actual scanner health, device inventory, manager records by source (count, first/last seen, sighting count, RSSI availability), $D_B,D_W$, separate estimates, $\rho$, combined estimate, uncertainty/range status, quality, filter/TTL, channel and hardware/driver metadata. Where privacy permits, retain research event traces separately with minimization and deletion controls.

| Trial family | Purpose and record |
|---|---|
| 0, 1, 2, 3, 5 people as feasible; repeated baselines | Empty-room false positives, low count behavior and repeatability; do not assume expected estimates |
| One vs multiple devices per person | Estimate within-radio $k_B,k_W$ effects |
| BT only, Wi-Fi only, both, neither | Source ablation, overlap and availability semantics |
| Radio-disabled/no-device participants | Estimate optional coverage $c$; without this do not correct to total attendance |
| Controlled arrivals/departures | TTL exit delay, response lag, temporal stability and oscillation |
| Known single-device address rotation | Characterize BT/Wi-Fi address churn without inferring identity from unknown MACs |
| Position/orientation/environment repeats | Test threshold sensitivity; do not turn RSSI into distance or count |
| Hardware capability/capture trials | Record monitor support, current channel and packet loss; separate from synthetic parser validation |

For actual $P_j$ and estimated $\widehat P_j$, record absolute error $|\widehat P_j-P_j|$. Aggregate:

$$
\mathrm{MAE}=\frac1N\sum_{j=1}^{N}|\widehat P_j-P_j|,\qquad
\mathrm{RMSE}=\sqrt{\frac1N\sum_{j=1}^{N}(\widehat P_j-P_j)^2}
$$

For $P_j>0$, percentage error is $100|\widehat P_j-P_j|/P_j$; omit it at zero and report false positives separately. Temporal metrics include mean absolute change between consecutive reports, standard deviation in fixed-ground-truth periods, transition detection delay, and oscillation count under a predeclared reversal threshold. For empirical ranges, report held-out coverage and mean width. Set acceptance targets before collecting evaluation data and label them goals, never measurements.

## Failure Modes

| Condition | Behavior |
|---|---|
| Neither radio confirmed available | Unavailable/insufficient; not zero people |
| One radio confirmed available | Its calibrated source estimate only; degraded quality and source-specific range if supported |
| No observations, scanner health unknown | Insufficient evidence |
| No observations after validated active window | May estimate zero radio-represented population; all-person zero still requires coverage evidence |
| Very few records or out-of-regime counts | Insufficient/low quality; do not extrapolate confidently |
| Rapid count changes | Recompute; avoid unvalidated smoothing, preserve transitions |
| Random address rotation | No identity claim; widen empirical uncertainty or mark low quality |
| Stale records | Respect manager TTL and current snapshot; do not resurrect expired state |
| Weak/unavailable RSSI | Existing threshold governs; unavailable is not zero; no distance inference |
| BT/Wi-Fi estimates disagree | Use calibrated overlap and range; flag if disagreement is outside supported data |
| Unusual device/person ratio or context shift | Out-of-domain/insufficient; recalibrate rather than clip to plausible-looking output |
| Wi-Fi client emits no Probe Request | Missing source evidence, not proof of no Wi-Fi users or people |

## Proposed API (Not Implemented)

Keep scanners and manager independent. Preserve snapshot input where practical, but pass actual scanner health and report time. A possible future shape is:

```c
typedef struct ps_estimator_config {
    double bt_records_per_person;
    double wifi_records_per_person;
    double overlap_fraction_of_smaller_population;
    bool all_people_coverage_available;
    double either_radio_coverage;
    /* versioned persistence and residual calibration */
} ps_estimator_config_t;

typedef struct ps_estimate {
    bool estimate_available;
    int estimated_people;
    bool uncertainty_range_available;
    int uncertainty_low;
    int uncertainty_high;
    ps_data_quality_t data_quality;
    /* per-source masses/estimates and calibration provenance */
} ps_estimate_t;

ps_status_t ps_estimator_init(ps_estimator_t *est,
                              const ps_estimator_config_t *config);
ps_status_t ps_estimator_estimate(ps_estimator_t *est,
                                  const ps_device_record_t *records,
                                  size_t count,
                                  bool bt_operational,
                                  bool wifi_operational,
                                  struct timespec report_time,
                                  ps_estimate_t *out);
void ps_estimator_reset(ps_estimator_t *est);
```

This is illustrative only. If version 1 omits hysteresis it can be stateless and `estimate()` need not retain state. Validate finite positive rates, $\rho\in[0,1]$, and coverage $c\in(0,1]$ when explicitly enabled. Output must distinguish unavailable from numeric zero and represent range availability separately. Data-quality enum changes and config/CLI calibration plumbing require separate implementation decisions. Concurrent input to one manager and actual scanner-health reporting are prerequisites, not optional extensions, for enabling the two-radio estimator.

## Hypothetical Worked Examples

Numbers below are invented arithmetic for explanation only, not measurements, validated defaults, or recommendations. Assume solely for these examples $k_B=2$, $k_W=1.5$, and $\rho=0.5$.

1. **Both radios:** $D_B=6$, $D_W=6$ gives $\widehat P_B=3$, $\widehat P_W=4$. Estimated intersection is $0.5\min(3,4)=1.5$, hence $\widehat P_{any}=3+4-1.5=5.5$. Round only at presentation; no deterministic device matching is implied. All-person count remains unavailable without $c$.
2. **BT only:** $D_B=4$ and Wi-Fi confirmed unavailable gives $\widehat P_B=2$. Do not add an assumed Wi-Fi population or apply overlap. Report a BT-represented estimate, degraded, with a BT-only range only if calibrated.
3. **No observations:** both radios confirmed operational for a validated window and $D_B=D_W=0$ may support zero represented people. It does not prove zero attendees. Without coverage calibration, total people is unavailable. If operational health is unknown, even zero represented population is unsupported.

## Interview Explanation

“I do not treat devices as people. I estimate the people represented by Bluetooth and Wi-Fi separately using per-radio device-to-person rates measured in labelled trials. Because the radios cannot reliably identify the same person, I combine those population estimates by subtracting a calibrated overlap fraction of the smaller represented population. The manager's TTL and persistence fields define current evidence, but repeat sightings do not add people and RSSI is not a person multiplier. Random MAC rotation and people without visible devices remain uncertainty sources. I report an empirical range only when held-out sessions support it.”

## Future Implementation Plan

1. Decide whether output means radio-represented people only or all people; the latter requires measured $c$.
2. Before enabling the two-radio estimator, make concurrent Bluetooth and Wi-Fi observations feed the same Observation Manager and report true scanner operational health (successful open/read over the evaluation window), not merely enabled configuration. Also decide whether to preserve Wi-Fi local-admin metadata. Add event history only if experiments show it is needed.
3. Pre-register ground-truth, privacy, supported-regime, calibration and held-out session protocol, plus project acceptance goals.
4. Implement per-radio record weights/rates and intermediate estimates with deterministic unit tests.
5. Fit and version $k_B,k_W$ and persistence rules on training trials; measure optional $\rho$ and $c$ only from suitable paired/person-labelled sessions.
6. Add overlap, zero/unavailable/out-of-domain behavior and held-out empirical uncertainty range.
7. Validate source ablations, multiple devices, rotation, entry/exit, TTL, unavailable RSSI, context shift, error and temporal metrics.
8. Validate actual monitor-mode RF capture on native Linux when hardware is available. WSL2 software tests cannot establish over-the-air or person-count performance.

## Scope Confirmation

- Estimator implemented: **NO**
- Bluetooth modified: **NO**
- Wi-Fi modified: **NO**
- Phase 5 started: **NO**
- Fabricated validation results: **NO**
