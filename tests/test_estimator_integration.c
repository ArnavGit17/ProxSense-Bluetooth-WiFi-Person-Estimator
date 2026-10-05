#include "ps_estimator.h"
#include "ps_obs_manager.h"
#include "ps_sim_scanner.h"

#include <assert.h>
#include <stdio.h>
#include <time.h>

int main(void)
{
    ps_sim_scanner_config_t simulation_config = {
        .scenario_file = NULL,
        .realtime = false
    };
    ps_scanner_t simulation_scanner;
    assert(ps_sim_scanner_init(&simulation_scanner, &simulation_config) == PS_OK);
    assert(ps_scanner_open(&simulation_scanner) == PS_OK);

    ps_obs_manager_t manager;
    ps_filter_config_t filter = ps_filter_config_default();
    assert(ps_obs_manager_init(&manager, &filter) == PS_OK);
    size_t delivered = 0U;
    for (;;) {
        ps_observation_t observation;
        ps_status_t status = ps_scanner_read_next(&simulation_scanner, &observation, 0);
        if (status == PS_ERR_TIMEOUT) break;
        assert(status == PS_OK);
        assert(observation.source == PS_SOURCE_BLUETOOTH ||
               observation.source == PS_SOURCE_WIFI);
        assert(ps_obs_manager_process(&manager, &observation) == PS_OK);
        delivered++;
    }
    assert(delivered == 6U);

    const ps_device_record_t *snapshot[PS_OBS_MANAGER_MAX_DEVICES];
    size_t count = 0U;
    assert(ps_obs_manager_snapshot(&manager, snapshot,
           PS_OBS_MANAGER_MAX_DEVICES, &count) == PS_OK);
    assert(count == 2U);
    ps_scanner_close(&simulation_scanner);

    ps_estimator_config_t estimator_config = ps_estimator_config_default();
    estimator_config.bt_records_per_person = 1.0;
    estimator_config.wifi_records_per_person = 1.0;
    estimator_config.overlap_fraction = 0.5;
    ps_estimator_t estimator;
    assert(ps_estimator_init(&estimator, &estimator_config) == PS_OK);
    struct timespec report_time;
    assert(clock_gettime(CLOCK_MONOTONIC, &report_time) == 0);
    ps_estimate_t result;
    assert(ps_estimator_run(&estimator, snapshot, count,
                            PS_SCANNER_SIMULATED, PS_SCANNER_SIMULATED,
                            &report_time, &result) == PS_OK);
    assert(result.estimate_available);
    assert(result.bt_records_seen == 1U);
    assert(result.wifi_records_seen == 1U);
    assert(result.bt_effective_record_mass == 1.0);
    assert(result.wifi_effective_record_mass == 1.0);
    assert(result.radio_represented_estimate == 1.5);
    assert(result.radio_represented_people == 2);
    assert(result.data_quality == PS_QUALITY_LOW);
    assert(!result.all_people_available);

    printf("PASS: test_simulation_manager_estimator_integration (6 simulated events)\n");
    return 0;
}
