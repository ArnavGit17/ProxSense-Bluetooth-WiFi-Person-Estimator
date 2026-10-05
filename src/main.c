#include "ps_config.h"
#include "ps_obs_manager.h"
#include "ps_estimator.h"
#include "ps_bt_scanner.h"
#include "ps_wifi_scanner.h"
#include "ps_sim_scanner.h"
#include "ps_log.h"

#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static int print_sysfs_interfaces(void)
{
    bool found_bluetooth = false;
    bool found_wifi = false;
    DIR *directory = opendir("/sys/class/bluetooth");
    printf("Bluetooth HCI controllers (sysfs presence only):\n");
    if (!directory) {
        printf("  unavailable: %s\n", strerror(errno));
    } else {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strncmp(entry->d_name, "hci", 3U) == 0) {
                printf("  %s\n", entry->d_name);
                found_bluetooth = true;
            }
        }
        (void)closedir(directory);
        if (!found_bluetooth) printf("  none found\n");
    }

    directory = opendir("/sys/class/net");
    printf("Wi-Fi nl80211 interfaces (phy80211 sysfs presence only):\n");
    if (!directory) {
        printf("  unavailable: %s\n", strerror(errno));
    } else {
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char phy_path[512];
            int length = snprintf(phy_path, sizeof(phy_path),
                                  "/sys/class/net/%s/phy80211", entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(phy_path)) continue;
            if (access(phy_path, F_OK) == 0) {
                printf("  %s\n", entry->d_name);
                found_wifi = true;
            }
        }
        (void)closedir(directory);
        if (!found_wifi) printf("  none found\n");
    }
    printf("Presence does not verify controller readiness, permissions, or monitor-mode support.\n");
    return EXIT_SUCCESS;
}

static const char *quality_str(ps_data_quality_t quality)
{
    switch (quality) {
        case PS_QUALITY_HIGH: return "HIGH";
        case PS_QUALITY_MEDIUM: return "MEDIUM";
        case PS_QUALITY_LOW: return "LOW";
        case PS_QUALITY_INSUFFICIENT: return "INSUFFICIENT";
        default: return "UNKNOWN";
    }
}

static void print_estimate(const ps_estimate_t *estimate, bool simulation)
{
    const char *tag = simulation ? "[SIMULATION] " : "";
    if (!estimate->estimate_available) {
        printf("%sEstimate: INSUFFICIENT EVIDENCE (scanner health/data unavailable)\n", tag);
        return;
    }

    const char *calibration = estimate->calibration_validated ?
                              "calibrated" : "UNCALIBRATED PLACEHOLDER";
    if (estimate->target == PS_ESTIMATE_ALL_PEOPLE) {
        printf("%sEstimated people: %d (all-people target; %s; quality: %s)\n",
               tag, estimate->estimated_people, calibration,
               quality_str(estimate->data_quality));
    } else {
        printf("%sRadio-represented population estimate: %d (%s; quality: %s)\n",
               tag, estimate->estimated_people, calibration,
               quality_str(estimate->data_quality));
    }

    if (estimate->uncertainty_range_available) {
        printf("%sUncertainty range: [%d, %d]\n", tag,
               estimate->uncertainty_low, estimate->uncertainty_high);
    } else {
        printf("%sUncertainty range: unavailable (no matching residual calibration)\n", tag);
    }

    PS_LOG_DEBUG("BT health=%d records=%zu mass=%.3f estimate=%.3f; "
                 "Wi-Fi health=%d records=%zu mass=%.3f estimate=%.3f; "
                 "random BT fraction=%.3f",
                 (int)estimate->bt_health, estimate->bt_records_seen,
                 estimate->bt_effective_record_mass,
                 estimate->bt_represented_estimate,
                 (int)estimate->wifi_health, estimate->wifi_records_seen,
                 estimate->wifi_effective_record_mass,
                 estimate->wifi_represented_estimate,
                 estimate->random_bt_fraction);
}

static bool read_is_fatal(ps_status_t status)
{
    return status != PS_OK && status != PS_ERR_TIMEOUT &&
           status != PS_ERR_MALFORMED && status != PS_ERR_UNSUPPORTED;
}

static void feed_scanner(ps_scanner_t **scanner,
                         ps_obs_manager_t *manager,
                         ps_status_t *health_window,
                         bool *polled_window)
{
    if (!scanner || !*scanner) return;
    ps_observation_t observation;
    ps_status_t status = ps_scanner_read_next(*scanner, &observation, 0);
    if (polled_window) *polled_window = true;
    if (status == PS_OK) {
        ps_status_t process_status = ps_obs_manager_process(manager, &observation);
        if (process_status != PS_OK && process_status != PS_ERR_FULL) {
            PS_LOG_WARN("Observation manager rejected a scanner record: %s",
                        ps_status_str(process_status));
        }
    } else if (read_is_fatal(status)) {
        PS_LOG_ERROR("%s scanner failed during capture: %s",
                     (*scanner)->source == PS_SOURCE_BLUETOOTH ? "Bluetooth" : "Wi-Fi",
                     ps_status_str(status));
        *health_window = status;
        ps_scanner_close(*scanner);
        *scanner = NULL;
    } else if (status != PS_ERR_TIMEOUT && status != PS_ERR_MALFORMED &&
               status != PS_ERR_UNSUPPORTED) {
        PS_LOG_WARN("Scanner read returned non-fatal status: %s", ps_status_str(status));
    }
}

static ps_scanner_health_t source_health(bool enabled,
                                         ps_scanner_t *scanner,
                                         ps_status_t window_status,
                                         bool polled_window,
                                         bool simulation)
{
    if (simulation) return PS_SCANNER_SIMULATED;
    if (!enabled) return PS_SCANNER_DISABLED;
    if (!scanner || window_status != PS_OK || !scanner->is_open || !polled_window) {
        return PS_SCANNER_UNAVAILABLE;
    }
    return PS_SCANNER_OPERATIONAL;
}

static void report_snapshot(ps_obs_manager_t *manager,
                            ps_estimator_t *estimator,
                            ps_scanner_health_t bt_health,
                            ps_scanner_health_t wifi_health,
                            bool simulation)
{
    (void)ps_obs_manager_expire(manager);
    const ps_device_record_t *snapshot[PS_OBS_MANAGER_MAX_DEVICES];
    size_t count = 0U;
    ps_status_t status = ps_obs_manager_snapshot(manager, snapshot,
                                                  PS_OBS_MANAGER_MAX_DEVICES,
                                                  &count);
    if (status != PS_OK) {
        PS_LOG_ERROR("Cannot snapshot observations: %s", ps_status_str(status));
        return;
    }

    struct timespec report_time;
    (void)clock_gettime(CLOCK_MONOTONIC, &report_time);
    ps_estimate_t estimate;
    status = ps_estimator_run(estimator, snapshot, count, bt_health, wifi_health,
                              &report_time, &estimate);
    if (status != PS_OK) {
        PS_LOG_ERROR("Estimator rejected current snapshot: %s", ps_status_str(status));
        return;
    }
    print_estimate(&estimate, simulation);
}

static void run_live_loop(ps_scanner_t *bt_scanner,
                          ps_scanner_t *wifi_scanner,
                          ps_obs_manager_t *manager,
                          ps_estimator_t *estimator,
                          const ps_config_t *config)
{
    struct timespec start_time;
    struct timespec last_report;
    (void)clock_gettime(CLOCK_MONOTONIC, &start_time);
    last_report = start_time;
    ps_status_t bt_window_status = bt_scanner ? PS_OK : PS_ERR_NO_HARDWARE;
    ps_status_t wifi_window_status = wifi_scanner ? PS_OK : PS_ERR_NO_HARDWARE;
    bool bt_polled_window = false;
    bool wifi_polled_window = false;
    const struct timespec poll_pause = { .tv_sec = 0, .tv_nsec = 20000000L };
    PS_LOG_INFO("Live event loop started; active scanners poll into one Observation Manager.");

    while (!g_stop) {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (config->duration_s > 0) {
            double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                             (double)(now.tv_nsec - start_time.tv_nsec) / 1.0e9;
            if (elapsed >= (double)config->duration_s) break;
        }

        feed_scanner(&bt_scanner, manager, &bt_window_status, &bt_polled_window);
        feed_scanner(&wifi_scanner, manager, &wifi_window_status, &wifi_polled_window);

        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        double since_report = (double)(now.tv_sec - last_report.tv_sec) +
                              (double)(now.tv_nsec - last_report.tv_nsec) / 1.0e9;
        if (since_report >= (double)config->report_interval_s) {
            ps_scanner_health_t bt_health = source_health(config->enable_bt,
                bt_scanner, bt_window_status, bt_polled_window, false);
            ps_scanner_health_t wifi_health = source_health(config->enable_wifi,
                wifi_scanner, wifi_window_status, wifi_polled_window, false);
            report_snapshot(manager, estimator, bt_health, wifi_health, false);
            last_report = now;
            bt_window_status = bt_scanner ? PS_OK : PS_ERR_NO_HARDWARE;
            wifi_window_status = wifi_scanner ? PS_OK : PS_ERR_NO_HARDWARE;
            bt_polled_window = false;
            wifi_polled_window = false;
        }
        (void)nanosleep(&poll_pause, NULL);
    }

    report_snapshot(manager, estimator,
                    source_health(config->enable_bt, bt_scanner, bt_window_status,
                                  bt_polled_window, false),
                    source_health(config->enable_wifi, wifi_scanner, wifi_window_status,
                                  wifi_polled_window, false),
                    false);
}

static void run_simulation_loop(ps_scanner_t *sim_scanner,
                                ps_obs_manager_t *manager,
                                ps_estimator_t *estimator,
                                const ps_config_t *config)
{
    struct timespec start_time;
    struct timespec last_report;
    (void)clock_gettime(CLOCK_MONOTONIC, &start_time);
    last_report = start_time;
    bool exhausted = false;

    while (!g_stop && !exhausted) {
        struct timespec now;
        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        if (config->duration_s > 0) {
            double elapsed = (double)(now.tv_sec - start_time.tv_sec) +
                             (double)(now.tv_nsec - start_time.tv_nsec) / 1.0e9;
            if (elapsed >= (double)config->duration_s) break;
        }
        ps_observation_t observation;
        ps_status_t status = ps_scanner_read_next(sim_scanner, &observation, 50);
        if (status == PS_OK) {
            ps_status_t process_status = ps_obs_manager_process(manager, &observation);
            if (process_status != PS_OK && process_status != PS_ERR_FULL) {
                PS_LOG_WARN("Observation manager rejected simulated record: %s",
                            ps_status_str(process_status));
            }
        } else if (status == PS_ERR_TIMEOUT) {
            exhausted = true;
        } else if (read_is_fatal(status)) {
            PS_LOG_ERROR("Simulation scanner failed: %s", ps_status_str(status));
            exhausted = true;
        } else if (status != PS_ERR_MALFORMED && status != PS_ERR_UNSUPPORTED) {
            PS_LOG_WARN("Simulation scanner returned status: %s", ps_status_str(status));
        }

        (void)clock_gettime(CLOCK_MONOTONIC, &now);
        double since_report = (double)(now.tv_sec - last_report.tv_sec) +
                              (double)(now.tv_nsec - last_report.tv_nsec) / 1.0e9;
        if (since_report >= (double)config->report_interval_s) {
            report_snapshot(manager, estimator, PS_SCANNER_SIMULATED,
                            PS_SCANNER_SIMULATED, true);
            last_report = now;
        }
    }
    report_snapshot(manager, estimator, PS_SCANNER_SIMULATED,
                    PS_SCANNER_SIMULATED, true);
}

static void close_if_initialized(ps_scanner_t *scanner, bool initialized)
{
    if (initialized) ps_scanner_close(scanner);
}

int main(int argc, char **argv)
{
    ps_config_t config;
    ps_status_t status = ps_config_parse_args(argc, argv, &config);
    if (status != PS_OK) return EXIT_FAILURE;

    PS_LOG_INFO("ProxSense %s starting.", "0.1.0-phase4b");
    struct sigaction signal_action;
    memset(&signal_action, 0, sizeof(signal_action));
    signal_action.sa_handler = handle_signal;
    (void)sigaction(SIGINT, &signal_action, NULL);
    (void)sigaction(SIGTERM, &signal_action, NULL);

    if (config.run_mode == PS_MODE_LIST_IFACES) {
        return print_sysfs_interfaces();
    }

    ps_estimator_t estimator;
    status = ps_estimator_init(&estimator, &config.estimator);
    if (status != PS_OK) {
        PS_LOG_ERROR("Estimator configuration is invalid.");
        return EXIT_FAILURE;
    }

    ps_obs_manager_t manager;
    status = ps_obs_manager_init(&manager, &config.filter);
    if (status != PS_OK) {
        PS_LOG_ERROR("Observation manager initialization failed: %s", ps_status_str(status));
        return EXIT_FAILURE;
    }

    if (config.run_mode == PS_MODE_SIMULATE) {
        ps_scanner_t simulation_scanner;
        status = ps_sim_scanner_init(&simulation_scanner, &config.sim);
        if (status != PS_OK) {
            PS_LOG_ERROR("Simulation scanner initialization failed: %s", ps_status_str(status));
            return EXIT_FAILURE;
        }
        PS_LOG_INFO("[SIMULATION] Scanner observations are synthetic, not RF health or live data.");
        status = ps_scanner_open(&simulation_scanner);
        if (status != PS_OK) {
            PS_LOG_ERROR("Simulation scanner open failed: %s", ps_status_str(status));
            ps_scanner_close(&simulation_scanner);
            return EXIT_FAILURE;
        }
        run_simulation_loop(&simulation_scanner, &manager, &estimator, &config);
        ps_scanner_close(&simulation_scanner);
        return EXIT_SUCCESS;
    }

    ps_scanner_t bt_storage;
    ps_scanner_t wifi_storage;
    ps_scanner_t *bt_scanner = NULL;
    ps_scanner_t *wifi_scanner = NULL;
    bool bt_initialized = false;
    bool wifi_initialized = false;

    if (config.enable_bt) {
        status = ps_bt_scanner_init(&bt_storage, &config.bt);
        if (status == PS_OK) {
            bt_initialized = true;
            status = ps_scanner_open(&bt_storage);
            if (status == PS_OK) bt_scanner = &bt_storage;
            else {
                PS_LOG_ERROR("Bluetooth scanner unavailable: %s", ps_status_str(status));
                ps_scanner_close(&bt_storage);
            }
        } else {
            PS_LOG_ERROR("Bluetooth scanner initialization failed: %s", ps_status_str(status));
        }
    }

    if (config.enable_wifi) {
        status = ps_wifi_scanner_init(&wifi_storage, &config.wifi);
        if (status == PS_OK) {
            wifi_initialized = true;
            status = ps_scanner_open(&wifi_storage);
            if (status == PS_OK) wifi_scanner = &wifi_storage;
            else {
                PS_LOG_ERROR("Wi-Fi scanner unavailable: %s", ps_status_str(status));
                ps_scanner_close(&wifi_storage);
            }
        } else {
            PS_LOG_ERROR("Wi-Fi scanner initialization failed: %s", ps_status_str(status));
        }
    }

    if (!bt_scanner && !wifi_scanner) {
        PS_LOG_ERROR("No live scanner successfully opened; no live estimate is available.");
        close_if_initialized(&bt_storage, bt_initialized);
        close_if_initialized(&wifi_storage, wifi_initialized);
        return EXIT_FAILURE;
    }

    run_live_loop(bt_scanner, wifi_scanner, &manager, &estimator, &config);
    close_if_initialized(&bt_storage, bt_initialized);
    close_if_initialized(&wifi_storage, wifi_initialized);
    PS_LOG_INFO("ProxSense exiting cleanly.");
    return EXIT_SUCCESS;
}
