# ProxSense — Bluetooth & Wi-Fi Based Nearby Person Estimator

ProxSense is a C application for Linux that estimates a nearby population from Bluetooth and Wi-Fi device observations. A detected radio address represents an observation of a device, not proof of a person. One person may carry multiple devices, and another may carry none that the scanners can observe; the estimator therefore requires deployment-specific calibration before its output can be interpreted as a real-world people estimate.

## Current Project Status

Core software implementation and deterministic validation are complete. Physical over-the-air Bluetooth/Wi-Fi validation and deployment-specific calibration remain pending because the current WSL2 environment does not expose the required Linux radio interfaces.

The estimator implementation and software test suite are complete. No physical RF validation, calibrated accuracy result, or real-world person-count performance is claimed.

## Implemented Components

- C11 implementation targeting Linux.
- Bluetooth Low Energy scanner using the BlueZ HCI interface.
- Wi-Fi monitor-mode scanner using nl80211 and AF_PACKET; valid Probe Request frames become observations.
- Shared Observation Manager for live Bluetooth and Wi-Fi scanner records.
- RSSI-threshold filtering, per-source/MAC deduplication, and TTL-based temporal tracking.
- Dual-source estimator with separate Bluetooth and Wi-Fi represented-population estimates, overlap handling, optional coverage correction, and a conditional uncertainty range.
- Deterministic simulation scanner and command-line interface.
- Deterministic unit and integration tests, including synthetic scanner/parser inputs.

## Architecture

The current processing pipeline is:

```text
Scanner Layer
    -> Observation Manager
    -> Filtering/Deduplication
    -> Temporal Tracking
    -> Person Estimator
    -> Estimated Population
```

In live dual-source mode, Bluetooth and Wi-Fi scanners are opened independently and, when available, feed observations into the same Observation Manager. Records remain distinguished by source and MAC address; matching addresses or similar timing do not establish that two radio observations belong to the same person. Filtering, deduplication, and TTL tracking are responsibilities of the manager. Simulation uses synthetic input and does not report physical scanner health.

## Estimation Methodology

The estimator operates on the manager's current post-expiry snapshot. It computes effective observation mass separately for Bluetooth and Wi-Fi. A configured transient weight may reduce a record's contribution until its source-specific persistence duration/sighting thresholds are met. Sighting count is used for persistence handling; it is not multiplied linearly into the person estimate.

Each operational radio produces a represented-population estimate by dividing its effective record mass by that radio's records-per-person parameter. When both physical scanners are operational, the estimates are combined using the calibrated overlap fraction `rho` of the smaller represented population:

`max(P_BT, P_WIFI) + (1 - rho) * min(P_BT, P_WIFI)`

This overlap model is an approximation; it does not identify or correlate devices across radios. If only one scanner is operational, the available radio estimate is used. RSSI is used by the Observation Manager as an observation filter, not as a direct distance or person-count multiplier.

All-person coverage correction is disabled by default. It requires explicit coverage configuration and calibration declaration, and both physical radio sources must be operational. An uncertainty range is available only when configured residual quantiles match the validated calibration regime and estimate target. It is an empirical residual range, not a confidence interval. Built-in estimator parameters are placeholders, not measured calibration values.

## Build and Test

On Linux, install GCC, GNU Make, and the BlueZ development library (`libbluetooth-dev` on Ubuntu), then run from the repository root:

```bash
make clean
make
make tests
```

The default `make` target builds `build/proxSense`. The `make tests` target builds and runs the `tests/test_*.c` test binaries. These software tests do not require physical radios and do not validate over-the-air capture.

## Running ProxSense

The verified command forms are:

```bash
./build/proxSense --help
./build/proxSense --simulate <scenario_file>
./build/proxSense --list-interfaces
./build/proxSense --live --duration <seconds>
```

For example, replay the repository's deterministic fixture with:

```bash
./build/proxSense --simulate tests/scenario_2_people.txt
```

The live command needs compatible Linux radio hardware and the permissions required by the scanner operations. Use `--no-bt` or `--no-wifi` to disable a scanner, `--wifi-interface <name>` to select a Wi-Fi interface, and `--interval <seconds>` to set the reporting interval. `--list-interfaces` reports sysfs interface presence only; it does not confirm controller readiness, permissions, or monitor-mode support.

## Simulation Mode

Simulation replays synthetic scenario events deterministically through the software pipeline. The scenario fixture is test input, not captured RF data. Simulation and its labelled fixture metadata do not demonstrate Bluetooth/Wi-Fi detection accuracy, physical person detection, or real-world population-count performance.

## Validation Status

The current verification run completed with:

- 7/7 test binaries passed.
- 109 individual PASS results.
- 0 FAIL results.
- 0 compiler warnings.
- Simulation, integration, and software validation completed.
- Physical over-the-air Bluetooth/Wi-Fi validation has **not** been performed.

These results establish software behavior for the tested cases only. They do not establish RF capture performance, calibration quality, or person-count accuracy.

## Hardware and Operational Limitations

In the current WSL2 environment, interface listing reported no Linux Bluetooth HCI controller and no physical nl80211-capable Wi-Fi interface. Accordingly, live mode reported both scanners unavailable and exited without a live estimate. This describes the current environment; it is not a claim about every WSL2 or Linux installation. Live mode does not substitute simulated observations when hardware is unavailable.

Live scanning requires a compatible Linux Bluetooth controller and, for Wi-Fi capture, an adapter/driver that supports nl80211 monitor mode. Wi-Fi capture currently follows the selected interface's channel and does not channel-hop. Scanner operation also depends on kernel support, driver behavior, permissions, and the radio environment.

## Limitations

- A person may carry multiple Bluetooth/Wi-Fi devices, while another person may carry no detectable device.
- Bluetooth and Wi-Fi may be disabled, asleep, non-discoverable, or silent during the observation window. Wi-Fi observations currently come from Probe Requests, which are intermittent and incomplete.
- Bluetooth random-address rotation is not resolved across identities. Wi-Fi privacy addresses may also rotate; the available local-address hint does not prove randomization. Cross-radio identity is not inferred.
- Nearby infrastructure and other non-person radio devices can contribute observations. There is no general infrastructure-device classifier in the Observation Manager.
- RSSI is noisy and affected by device transmit power, antenna orientation, body absorption, multipath, interference, walls, and other environmental obstacles. It is not a direct distance measurement.
- Device visibility and the number of devices carried per person vary by population, deployment, time, and environment. Records-per-person, overlap, optional coverage correction, and residual calibration must be established for the intended deployment.
- The current WSL2 environment does not expose the Linux radio interfaces required for physical live validation.

## Repository Structure

```text
ProxSense/
├── config/                         # Empty in the current workspace
├── docs/
│   ├── ARCHITECTURE.md
│   ├── ASSUMPTIONS_AND_LIMITATIONS.md
│   ├── DETECTION_METHODOLOGY.md
│   ├── PHASE_4A_ESTIMATOR_DESIGN.md
│   └── VALIDATION.md
├── include/                        # Public and private C interfaces
├── src/                            # Application, scanners, manager, estimator, CLI
├── tests/                          # Seven test sources and scenario fixture
├── build/                          # Generated application and test binaries
├── Makefile
├── README.md
└── test_bt.c                       # Root-level test source
```

The `make tests` target discovers `tests/test_*.c`; the root-level `test_bt.c` is not one of those seven test binaries.

## Further Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Detection methodology](docs/DETECTION_METHODOLOGY.md)
- [Validation strategy](docs/VALIDATION.md)
- [Assumptions and limitations](docs/ASSUMPTIONS_AND_LIMITATIONS.md)
- [Phase 4A estimator design](docs/PHASE_4A_ESTIMATOR_DESIGN.md)
