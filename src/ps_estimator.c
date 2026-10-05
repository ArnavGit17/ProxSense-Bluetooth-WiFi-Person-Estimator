#include "ps_estimator.h"
#include "ps_log.h"

#include <math.h>
#include <string.h>

static bool health_is_operational(ps_scanner_health_t health)
{
    return health == PS_SCANNER_OPERATIONAL || health == PS_SCANNER_SIMULATED;
}

static double timespec_delta_s(const struct timespec *start,
                               const struct timespec *end)
{
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1.0e9;
}

bool ps_estimator_config_is_valid(const ps_estimator_config_t *config)
{
    if (!config) return false;
    if (!isfinite(config->bt_records_per_person) ||
        config->bt_records_per_person <= 0.0 ||
        !isfinite(config->wifi_records_per_person) ||
        config->wifi_records_per_person <= 0.0 ||
        !isfinite(config->overlap_fraction) ||
        config->overlap_fraction < 0.0 || config->overlap_fraction > 1.0 ||
        !isfinite(config->bt_transient_weight) ||
        config->bt_transient_weight < 0.0 || config->bt_transient_weight > 1.0 ||
        !isfinite(config->wifi_transient_weight) ||
        config->wifi_transient_weight < 0.0 || config->wifi_transient_weight > 1.0 ||
        !isfinite(config->bt_min_persistence_s) ||
        config->bt_min_persistence_s < 0.0 ||
        !isfinite(config->wifi_min_persistence_s) ||
        config->wifi_min_persistence_s < 0.0 ||
        config->bt_min_sightings == 0U || config->wifi_min_sightings == 0U) {
        return false;
    }

    if (config->all_people_coverage_enabled &&
        (!config->calibration_validated ||
         !isfinite(config->either_radio_coverage) ||
         config->either_radio_coverage <= 0.0 ||
         config->either_radio_coverage > 1.0)) {
        return false;
    }

    if (config->uncertainty_range_enabled &&
        (!config->calibration_validated ||
         !isfinite(config->residual_q_low) ||
         !isfinite(config->residual_q_high) ||
         fabs(config->residual_q_low) > (double)INT32_MAX ||
         fabs(config->residual_q_high) > (double)INT32_MAX ||
         config->residual_q_low > config->residual_q_high)) {
        return false;
    }
    if (config->uncertainty_range_enabled && config->residuals_for_all_people &&
        !config->all_people_coverage_enabled) {
        return false;
    }

    if (config->address_rotation_validated && !config->calibration_validated) {
        return false;
    }
    return true;
}

static int round_people(double value)
{
    return (int)floor(value + 0.5);
}

ps_estimator_config_t ps_estimator_config_default(void)
{
    return (ps_estimator_config_t){
        .bt_records_per_person = 1.5, /* Placeholder only; not calibrated. */
        .wifi_records_per_person = 1.0, /* Placeholder only; not calibrated. */
        .overlap_fraction = 0.5, /* Placeholder only; not calibrated. */
        .calibration_validated = false,
        .address_rotation_validated = false,
        .all_people_coverage_enabled = false,
        .either_radio_coverage = 0.5, /* Placeholder; ignored unless enabled. */
        .bt_transient_weight = 1.0,
        .wifi_transient_weight = 1.0,
        .bt_min_persistence_s = 0.0,
        .wifi_min_persistence_s = 0.0,
        .bt_min_sightings = 1U,
        .wifi_min_sightings = 1U,
        .uncertainty_range_enabled = false,
        .residuals_for_all_people = false,
        .residual_q_low = 0.0,
        .residual_q_high = 0.0
    };
}

ps_status_t ps_estimator_init(ps_estimator_t *est,
                              const ps_estimator_config_t *config)
{
    if (!est) return PS_ERR_INVALID_ARG;
    ps_estimator_config_t selected = config ? *config : ps_estimator_config_default();
    if (!ps_estimator_config_is_valid(&selected)) return PS_ERR_INVALID_ARG;
    est->config = selected;
    if (!est->config.calibration_validated) {
        PS_LOG_WARN("Estimator calibration uses explicit placeholders; count output is unvalidated.");
    }
    return PS_OK;
}

ps_status_t ps_estimator_run(const ps_estimator_t *est,
                             const ps_device_record_t **records,
                             size_t count,
                             ps_scanner_health_t bt_health,
                             ps_scanner_health_t wifi_health,
                             const struct timespec *report_time,
                             ps_estimate_t *out)
{
    if (!est || !report_time || !out || (count > 0U && !records)) {
        return PS_ERR_INVALID_ARG;
    }
    if (bt_health < PS_SCANNER_DISABLED || bt_health > PS_SCANNER_SIMULATED ||
        wifi_health < PS_SCANNER_DISABLED || wifi_health > PS_SCANNER_SIMULATED) {
        return PS_ERR_INVALID_ARG;
    }

    *out = (ps_estimate_t){
        .estimate_available = false,
        .estimated_people = PS_ESTIMATE_UNAVAILABLE,
        .target = PS_ESTIMATE_RADIO_REPRESENTED,
        .radio_represented_available = false,
        .all_people_available = false,
        .all_people_count = PS_ESTIMATE_UNAVAILABLE,
        .uncertainty_range_available = false,
        .uncertainty_low = PS_ESTIMATE_UNAVAILABLE,
        .uncertainty_high = PS_ESTIMATE_UNAVAILABLE,
        .data_quality = PS_QUALITY_INSUFFICIENT,
        .bt_health = bt_health,
        .wifi_health = wifi_health,
        .calibration_validated = est->config.calibration_validated
    };

    bool bt_operational = health_is_operational(bt_health);
    bool wifi_operational = health_is_operational(wifi_health);
    if (!bt_operational && !wifi_operational) return PS_OK;

    size_t random_bt_records = 0U;
    for (size_t i = 0U; i < count; ++i) {
        if (!records[i]) return PS_ERR_INVALID_ARG;
        const ps_device_record_t *record = records[i];
        double duration_s = timespec_delta_s(&record->first_seen, &record->last_seen);
        double age_s = timespec_delta_s(&record->last_seen, report_time);
        if (!isfinite(duration_s) || !isfinite(age_s) || duration_s < 0.0 || age_s < 0.0) {
            return PS_ERR_MALFORMED;
        }

        double weight = 1.0;
        if (record->source == PS_SOURCE_BLUETOOTH && bt_operational) {
            out->bt_records_seen++;
            if (record->is_random_bt) random_bt_records++;
            if (duration_s < est->config.bt_min_persistence_s ||
                record->sighting_count < est->config.bt_min_sightings) {
                weight = est->config.bt_transient_weight;
            }
            out->bt_effective_record_mass += weight;
        } else if (record->source == PS_SOURCE_WIFI && wifi_operational) {
            out->wifi_records_seen++;
            if (duration_s < est->config.wifi_min_persistence_s ||
                record->sighting_count < est->config.wifi_min_sightings) {
                weight = est->config.wifi_transient_weight;
            }
            out->wifi_effective_record_mass += weight;
        }
    }

    if (out->bt_records_seen > 0U) {
        out->random_bt_fraction = (double)random_bt_records /
                                  (double)out->bt_records_seen;
    }
    out->bt_represented_estimate = bt_operational ?
        out->bt_effective_record_mass / est->config.bt_records_per_person : 0.0;
    out->wifi_represented_estimate = wifi_operational ?
        out->wifi_effective_record_mass / est->config.wifi_records_per_person : 0.0;

    double represented;
    if (bt_operational && wifi_operational) {
        double smaller = fmin(out->bt_represented_estimate,
                              out->wifi_represented_estimate);
        double larger = fmax(out->bt_represented_estimate,
                             out->wifi_represented_estimate);
        represented = larger + (1.0 - est->config.overlap_fraction) * smaller;
    } else {
        represented = bt_operational ? out->bt_represented_estimate :
                                       out->wifi_represented_estimate;
    }

    if (!isfinite(represented) || represented < 0.0 ||
        represented > (double)INT32_MAX - 0.5) {
        return PS_ERR_MALFORMED;
    }

    out->radio_represented_available = true;
    out->radio_represented_estimate = represented;
    out->radio_represented_people = round_people(represented);
    out->estimate_available = true;
    out->estimated_people = out->radio_represented_people;

    bool both_physical_operational =
        bt_health == PS_SCANNER_OPERATIONAL &&
        wifi_health == PS_SCANNER_OPERATIONAL;
    if (est->config.all_people_coverage_enabled && both_physical_operational) {
        double all_people = represented / est->config.either_radio_coverage;
        if (!isfinite(all_people) || all_people < 0.0 ||
            all_people > (double)INT32_MAX - 0.5) {
            return PS_ERR_MALFORMED;
        }
        out->all_people_available = true;
        out->all_people_estimate = all_people;
        out->all_people_count = round_people(all_people);
        out->target = PS_ESTIMATE_ALL_PEOPLE;
        out->estimated_people = out->all_people_count;
    }

    bool range_target_matches =
        est->config.uncertainty_range_enabled &&
        ((out->target == PS_ESTIMATE_ALL_PEOPLE && est->config.residuals_for_all_people) ||
         (out->target == PS_ESTIMATE_RADIO_REPRESENTED && !est->config.residuals_for_all_people));
    /* A shared residual pair is only accepted for the jointly measured two-radio regime. */
    if (range_target_matches && both_physical_operational) {
        double point = out->target == PS_ESTIMATE_ALL_PEOPLE ?
                       out->all_people_estimate : represented;
        double low = fmax(0.0, floor(point - est->config.residual_q_high));
        double high = fmax(0.0, ceil(point - est->config.residual_q_low));
        if (low <= high && high <= (double)INT32_MAX) {
            out->uncertainty_range_available = true;
            out->uncertainty_low = (int)low;
            out->uncertainty_high = (int)high;
        }
    }

    if (bt_health == PS_SCANNER_SIMULATED || wifi_health == PS_SCANNER_SIMULATED ||
        !est->config.calibration_validated) {
        out->data_quality = PS_QUALITY_LOW;
    } else if (both_physical_operational && out->uncertainty_range_available &&
               est->config.address_rotation_validated) {
        out->data_quality = PS_QUALITY_HIGH;
    } else {
        out->data_quality = PS_QUALITY_MEDIUM;
    }

    PS_LOG_DEBUG("Estimator: BT mass=%.3f, Wi-Fi mass=%.3f, represented=%.3f, target=%s",
                 out->bt_effective_record_mass,
                 out->wifi_effective_record_mass,
                 represented,
                 out->target == PS_ESTIMATE_ALL_PEOPLE ? "all-people" : "radio-represented");
    return PS_OK;
}
