#include "ps_estimator.h"
#include "ps_obs_manager.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_REPORT_TIME ((struct timespec){ .tv_sec = 1000, .tv_nsec = 0 })

static ps_estimator_config_t calibrated_config(void)
{
    ps_estimator_config_t config = ps_estimator_config_default();
    config.bt_records_per_person = 1.0;
    config.wifi_records_per_person = 1.0;
    config.overlap_fraction = 0.5;
    config.calibration_validated = true; /* Synthetic fixture only. */
    config.address_rotation_validated = true;
    return config;
}

static ps_device_record_t make_record(ps_source_t source,
                                      unsigned int index,
                                      int first_seen_s,
                                      int last_seen_s,
                                      uint32_t sightings)
{
    ps_device_record_t record;
    memset(&record, 0, sizeof(record));
    record.source = source;
    (void)snprintf(record.mac.str, sizeof(record.mac.str),
                   "02:00:00:00:00:%02X", index & 0xffU);
    record.first_seen.tv_sec = (time_t)first_seen_s;
    record.last_seen.tv_sec = (time_t)last_seen_s;
    record.sighting_count = sightings;
    record.rssi_last_dbm = PS_RSSI_UNAVAILABLE;
    record.rssi_peak_dbm = PS_RSSI_UNAVAILABLE;
    return record;
}

static ps_estimate_t run_estimate(ps_estimator_config_t config,
                                  const ps_device_record_t **records,
                                  size_t count,
                                  ps_scanner_health_t bt_health,
                                  ps_scanner_health_t wifi_health)
{
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_OK);
    ps_estimate_t estimate;
    assert(ps_estimator_run(&estimator, records, count, bt_health, wifi_health,
                            &TEST_REPORT_TIME, &estimate) == PS_OK);
    return estimate;
}

static int test_empty_snapshot_both_operational(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.estimate_available);
    assert(estimate.radio_represented_people == 0);
    printf("PASS: test_empty_snapshot_both_operational\n");
    return 0;
}

static int test_zero_records_both_operational(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.estimate_available && estimate.estimated_people == 0);
    printf("PASS: test_zero_records_both_operational\n");
    return 0;
}

static int test_zero_records_health_unknown(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_HEALTH_UNKNOWN,
                                          PS_SCANNER_HEALTH_UNKNOWN);
    assert(!estimate.estimate_available);
    assert(estimate.estimated_people == PS_ESTIMATE_UNAVAILABLE);
    printf("PASS: test_zero_records_health_unknown\n");
    return 0;
}

static int test_bluetooth_only_estimate(void)
{
    ps_device_record_t first = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    ps_device_record_t second = make_record(PS_SOURCE_BLUETOOTH, 2U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &first, &second };
    ps_estimate_t estimate = run_estimate(calibrated_config(), records, 2U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_UNAVAILABLE);
    assert(estimate.radio_represented_estimate == 2.0);
    assert(estimate.wifi_records_seen == 0U);
    assert(estimate.data_quality == PS_QUALITY_MEDIUM);
    printf("PASS: test_bluetooth_only_estimate\n");
    return 0;
}

static int test_wifi_only_estimate(void)
{
    ps_device_record_t first = make_record(PS_SOURCE_WIFI, 1U, 990, 999, 1U);
    ps_device_record_t second = make_record(PS_SOURCE_WIFI, 2U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &first, &second };
    ps_estimate_t estimate = run_estimate(calibrated_config(), records, 2U,
                                          PS_SCANNER_DISABLED,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.radio_represented_estimate == 2.0);
    assert(estimate.bt_records_seen == 0U);
    printf("PASS: test_wifi_only_estimate\n");
    return 0;
}

static int test_both_radio_estimate(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_records_per_person = 2.0;
    config.wifi_records_per_person = 3.0;
    ps_device_record_t bt[4];
    ps_device_record_t wifi[6];
    const ps_device_record_t *records[10];
    for (size_t i = 0U; i < 4U; ++i) {
        bt[i] = make_record(PS_SOURCE_BLUETOOTH, (unsigned int)i, 990, 999, 1U);
        records[i] = &bt[i];
    }
    for (size_t i = 0U; i < 6U; ++i) {
        wifi[i] = make_record(PS_SOURCE_WIFI, (unsigned int)(i + 10U), 990, 999, 1U);
        records[i + 4U] = &wifi[i];
    }
    ps_estimate_t estimate = run_estimate(config, records, 10U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.bt_represented_estimate == 2.0);
    assert(estimate.wifi_represented_estimate == 2.0);
    assert(estimate.radio_represented_estimate == 3.0);
    printf("PASS: test_both_radio_estimate\n");
    return 0;
}

static ps_estimate_t run_two_radio_masses(double rho)
{
    ps_estimator_config_t config = calibrated_config();
    config.overlap_fraction = rho;
    config.bt_records_per_person = 1.0;
    config.wifi_records_per_person = 1.0;
    ps_device_record_t bt[2];
    ps_device_record_t wifi[2];
    const ps_device_record_t *records[4];
    for (size_t i = 0U; i < 2U; ++i) {
        bt[i] = make_record(PS_SOURCE_BLUETOOTH, (unsigned int)i, 990, 999, 1U);
        wifi[i] = make_record(PS_SOURCE_WIFI, (unsigned int)(i + 20U), 990, 999, 1U);
        records[i] = &bt[i];
        records[i + 2U] = &wifi[i];
    }
    return run_estimate(config, records, 4U,
                        PS_SCANNER_OPERATIONAL, PS_SCANNER_OPERATIONAL);
}

static int test_rho_zero(void)
{
    assert(run_two_radio_masses(0.0).radio_represented_estimate == 4.0);
    printf("PASS: test_rho_zero\n");
    return 0;
}

static int test_rho_one(void)
{
    assert(run_two_radio_masses(1.0).radio_represented_estimate == 2.0);
    printf("PASS: test_rho_one\n");
    return 0;
}

static int test_rho_intermediate(void)
{
    assert(run_two_radio_masses(0.25).radio_represented_estimate == 3.5);
    printf("PASS: test_rho_intermediate\n");
    return 0;
}

static int test_different_source_rates(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_records_per_person = 2.0;
    config.wifi_records_per_person = 4.0;
    ps_device_record_t bt[4];
    ps_device_record_t wifi[4];
    const ps_device_record_t *records[8];
    for (size_t i = 0U; i < 4U; ++i) {
        bt[i] = make_record(PS_SOURCE_BLUETOOTH, (unsigned int)i, 990, 999, 1U);
        wifi[i] = make_record(PS_SOURCE_WIFI, (unsigned int)(i + 30U), 990, 999, 1U);
        records[i] = &bt[i];
        records[i + 4U] = &wifi[i];
    }
    ps_estimate_t estimate = run_estimate(config, records, 8U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.bt_represented_estimate == 2.0);
    assert(estimate.wifi_represented_estimate == 1.0);
    printf("PASS: test_different_source_rates\n");
    return 0;
}

static int test_persistence_weighting(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_transient_weight = 0.25;
    config.bt_min_persistence_s = 5.0;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 998, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_DISABLED);
    assert(estimate.bt_effective_record_mass == 0.25);
    printf("PASS: test_persistence_weighting\n");
    return 0;
}

static int test_persistence_threshold(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_min_persistence_s = 5.0;
    config.bt_min_sightings = 3U;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 989, 999, 3U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_DISABLED);
    assert(estimate.bt_effective_record_mass == 1.0);
    printf("PASS: test_persistence_threshold\n");
    return 0;
}

static int test_sighting_count_not_linear(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_records_per_person = 1.0;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 100U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_DISABLED);
    assert(estimate.bt_effective_record_mass == 1.0);
    assert(estimate.radio_represented_estimate == 1.0);
    printf("PASS: test_sighting_count_not_linear\n");
    return 0;
}

static int test_unavailable_rssi(void)
{
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(calibrated_config(), records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_DISABLED);
    assert(record.rssi_last_dbm == PS_RSSI_UNAVAILABLE);
    assert(estimate.radio_represented_estimate == 1.0);
    printf("PASS: test_unavailable_rssi\n");
    return 0;
}

static int test_stale_records_expired_by_manager(void)
{
    ps_obs_manager_t manager;
    ps_filter_config_t filter = ps_filter_config_default();
    filter.device_ttl_s = 1;
    assert(ps_obs_manager_init(&manager, &filter) == PS_OK);
    ps_observation_t observation;
    ps_observation_init(&observation);
    observation.source = PS_SOURCE_BLUETOOTH;
    (void)strcpy(observation.mac.str, "02:00:00:00:00:01");
    assert(ps_obs_manager_process(&manager, &observation) == PS_OK);
    manager.table[0].last_seen.tv_sec -= 10;
    assert(ps_obs_manager_expire(&manager) == 1U);
    const ps_device_record_t *snapshot[PS_OBS_MANAGER_MAX_DEVICES];
    size_t count = 0U;
    assert(ps_obs_manager_snapshot(&manager, snapshot,
           PS_OBS_MANAGER_MAX_DEVICES, &count) == PS_OK);
    assert(count == 0U);
    ps_estimate_t estimate = run_estimate(calibrated_config(), snapshot, count,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.radio_represented_people == 0);
    printf("PASS: test_stale_records_expired_by_manager\n");
    return 0;
}

static int test_random_bluetooth_diagnostic(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.address_rotation_validated = false;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    record.is_random_bt = true;
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.random_bt_fraction == 1.0);
    assert(estimate.bt_effective_record_mass == 1.0);
    assert(estimate.data_quality == PS_QUALITY_MEDIUM);
    printf("PASS: test_random_bluetooth_diagnostic\n");
    return 0;
}

static int test_invalid_k(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_records_per_person = 0.0;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_ERR_INVALID_ARG);
    printf("PASS: test_invalid_k\n");
    return 0;
}

static int test_invalid_rho(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.overlap_fraction = 1.01;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_ERR_INVALID_ARG);
    printf("PASS: test_invalid_rho\n");
    return 0;
}

static int test_invalid_coverage(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.all_people_coverage_enabled = true;
    config.either_radio_coverage = 0.0;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_ERR_INVALID_ARG);
    printf("PASS: test_invalid_coverage\n");
    return 0;
}

static int test_coverage_disabled(void)
{
    ps_estimator_config_t config = calibrated_config();
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(!estimate.all_people_available);
    assert(estimate.target == PS_ESTIMATE_RADIO_REPRESENTED);
    printf("PASS: test_coverage_disabled\n");
    return 0;
}

static int test_coverage_enabled(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.all_people_coverage_enabled = true;
    config.either_radio_coverage = 0.5;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.radio_represented_estimate == 1.0);
    assert(estimate.all_people_available);
    assert(estimate.all_people_estimate == 2.0);
    assert(estimate.target == PS_ESTIMATE_ALL_PEOPLE);
    printf("PASS: test_coverage_enabled\n");
    return 0;
}

static int test_unavailable_scanner(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_UNAVAILABLE,
                                          PS_SCANNER_DISABLED);
    assert(!estimate.estimate_available);
    printf("PASS: test_unavailable_scanner\n");
    return 0;
}

static int test_uncertainty_unavailable(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(!estimate.uncertainty_range_available);
    printf("PASS: test_uncertainty_unavailable\n");
    return 0;
}

static int test_uncertainty_range_calculation(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.uncertainty_range_enabled = true;
    config.residual_q_low = -1.0;
    config.residual_q_high = 2.0;
    ps_device_record_t records_data[2];
    records_data[0] = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    records_data[1] = make_record(PS_SOURCE_BLUETOOTH, 2U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &records_data[0], &records_data[1] };
    ps_estimate_t estimate = run_estimate(config, records, 2U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.uncertainty_range_available);
    assert(estimate.uncertainty_low == 0);
    assert(estimate.uncertainty_high == 3);
    printf("PASS: test_uncertainty_range_calculation\n");
    return 0;
}

static int test_negative_uncertainty_lower_clamped(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.uncertainty_range_enabled = true;
    config.residual_q_low = 0.0;
    config.residual_q_high = 5.0;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.uncertainty_low == 0);
    assert(estimate.uncertainty_high == 1);
    printf("PASS: test_negative_uncertainty_lower_clamped\n");
    return 0;
}

static int test_deterministic_half_up_rounding(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.overlap_fraction = 0.5;
    ps_device_record_t bt[2];
    ps_device_record_t wifi[1];
    const ps_device_record_t *records[3];
    bt[0] = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    bt[1] = make_record(PS_SOURCE_BLUETOOTH, 2U, 990, 999, 1U);
    wifi[0] = make_record(PS_SOURCE_WIFI, 3U, 990, 999, 1U);
    records[0] = &bt[0]; records[1] = &bt[1]; records[2] = &wifi[0];
    ps_estimate_t estimate = run_estimate(config, records, 3U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.radio_represented_estimate == 2.5);
    assert(estimate.radio_represented_people == 3);
    printf("PASS: test_deterministic_half_up_rounding\n");
    return 0;
}

static int test_insufficient_unknown_health(void)
{
    ps_estimate_t estimate = run_estimate(calibrated_config(), NULL, 0U,
                                          PS_SCANNER_HEALTH_UNKNOWN,
                                          PS_SCANNER_DISABLED);
    assert(!estimate.estimate_available);
    assert(estimate.data_quality == PS_QUALITY_INSUFFICIENT);
    printf("PASS: test_insufficient_unknown_health\n");
    return 0;
}

static int test_high_quality_requires_calibration_range_and_rotation(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.uncertainty_range_enabled = true;
    config.address_rotation_validated = true;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_OPERATIONAL);
    assert(estimate.data_quality == PS_QUALITY_HIGH);
    printf("PASS: test_high_quality_requires_calibration_range_and_rotation\n");
    return 0;
}

static int test_coverage_not_applied_to_single_radio(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.all_people_coverage_enabled = true;
    config.either_radio_coverage = 0.5;
    ps_device_record_t record = make_record(PS_SOURCE_BLUETOOTH, 1U, 990, 999, 1U);
    const ps_device_record_t *records[] = { &record };
    ps_estimate_t estimate = run_estimate(config, records, 1U,
                                          PS_SCANNER_OPERATIONAL,
                                          PS_SCANNER_UNAVAILABLE);
    assert(estimate.radio_represented_available);
    assert(!estimate.all_people_available);
    assert(estimate.target == PS_ESTIMATE_RADIO_REPRESENTED);
    printf("PASS: test_coverage_not_applied_to_single_radio\n");
    return 0;
}

static int test_uncertainty_requires_validated_calibration(void)
{
    ps_estimator_config_t config = ps_estimator_config_default();
    config.uncertainty_range_enabled = true;
    config.residual_q_low = -1.0;
    config.residual_q_high = 1.0;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_ERR_INVALID_ARG);
    printf("PASS: test_uncertainty_requires_validated_calibration\n");
    return 0;
}

static int test_invalid_persistence_weight(void)
{
    ps_estimator_config_t config = calibrated_config();
    config.bt_transient_weight = -0.1;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &config) == PS_ERR_INVALID_ARG);
    printf("PASS: test_invalid_persistence_weight\n");
    return 0;
}

int main(void)
{
    printf("=== test_estimator ===\n");
    test_empty_snapshot_both_operational();
    test_zero_records_both_operational();
    test_zero_records_health_unknown();
    test_bluetooth_only_estimate();
    test_wifi_only_estimate();
    test_both_radio_estimate();
    test_rho_zero();
    test_rho_one();
    test_rho_intermediate();
    test_different_source_rates();
    test_persistence_weighting();
    test_persistence_threshold();
    test_sighting_count_not_linear();
    test_unavailable_rssi();
    test_stale_records_expired_by_manager();
    test_random_bluetooth_diagnostic();
    test_invalid_k();
    test_invalid_rho();
    test_invalid_coverage();
    test_coverage_disabled();
    test_coverage_enabled();
    test_unavailable_scanner();
    test_uncertainty_unavailable();
    test_uncertainty_range_calculation();
    test_negative_uncertainty_lower_clamped();
    test_deterministic_half_up_rounding();
    test_insufficient_unknown_health();
    test_high_quality_requires_calibration_range_and_rotation();
    test_coverage_not_applied_to_single_radio();
    test_uncertainty_requires_validated_calibration();
    test_invalid_persistence_weight();
    printf("All estimator tests PASSED.\n");
    return 0;
}
