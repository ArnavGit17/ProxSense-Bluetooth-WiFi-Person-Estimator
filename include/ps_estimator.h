#ifndef PS_ESTIMATOR_H
#define PS_ESTIMATOR_H

#include "ps_obs_manager.h"
#include "ps_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum ps_data_quality {
    PS_QUALITY_HIGH = 0,
    PS_QUALITY_MEDIUM = 1,
    PS_QUALITY_LOW = 2,
    PS_QUALITY_INSUFFICIENT = 3
} ps_data_quality_t;

/* Compatibility names for the earlier CLI/API vocabulary. */
#define PS_QUALITY_GOOD PS_QUALITY_HIGH
#define PS_QUALITY_DEGRADED PS_QUALITY_MEDIUM
#define PS_QUALITY_UNAVAILABLE PS_QUALITY_INSUFFICIENT

typedef enum ps_scanner_health {
    PS_SCANNER_DISABLED = 0,       /* Not enabled by configuration. */
    PS_SCANNER_OPERATIONAL = 1,    /* Opened and remained free of fatal read errors in window. */
    PS_SCANNER_UNAVAILABLE = 2,    /* Enabled but failed to open or capture. */
    PS_SCANNER_HEALTH_UNKNOWN = 3, /* No trustworthy health evidence. */
    PS_SCANNER_SIMULATED = 4       /* Synthetic input; never hardware health. */
} ps_scanner_health_t;

typedef enum ps_estimate_target {
    PS_ESTIMATE_RADIO_REPRESENTED = 0,
    PS_ESTIMATE_ALL_PEOPLE = 1
} ps_estimate_target_t;

typedef struct ps_estimator_config {
    double bt_records_per_person;       /* Placeholder until calibrated. */
    double wifi_records_per_person;     /* Placeholder until calibrated. */
    double overlap_fraction;            /* rho: overlap share of smaller represented population. */
    bool calibration_validated;         /* Caller assertion; not independently verified here. */
    bool address_rotation_validated;    /* BT and Wi-Fi rotation regime covered by calibration. */

    bool all_people_coverage_enabled;
    double either_radio_coverage;        /* c; used only when explicitly enabled. */

    double bt_transient_weight;          /* q_B; sighting count is only a persistence threshold. */
    double wifi_transient_weight;        /* q_W; never a linear person multiplier. */
    double bt_min_persistence_s;
    double wifi_min_persistence_s;
    uint32_t bt_min_sightings;
    uint32_t wifi_min_sightings;

    bool uncertainty_range_enabled;
    bool residuals_for_all_people;
    double residual_q_low;               /* q_L for e = estimate - actual. */
    double residual_q_high;              /* q_U for e = estimate - actual. */
} ps_estimator_config_t;

typedef struct ps_estimate {
    bool estimate_available;
    ps_estimate_target_t target;
    int estimated_people;                /* Rounded target value; zero differs from unavailable. */

    bool radio_represented_available;
    double radio_represented_estimate;
    int radio_represented_people;

    bool all_people_available;            /* Requires explicit coverage and both physical sources. */
    double all_people_estimate;
    int all_people_count;

    bool uncertainty_range_available;
    int uncertainty_low;
    int uncertainty_high;
    ps_data_quality_t data_quality;

    ps_scanner_health_t bt_health;
    ps_scanner_health_t wifi_health;
    bool calibration_validated;
    size_t bt_records_seen;
    size_t wifi_records_seen;
    double bt_effective_record_mass;
    double wifi_effective_record_mass;
    double bt_represented_estimate;
    double wifi_represented_estimate;
    double random_bt_fraction;
} ps_estimate_t;

#define PS_ESTIMATE_UNAVAILABLE (-1)

typedef struct ps_estimator {
    ps_estimator_config_t config;
} ps_estimator_t;

ps_estimator_config_t ps_estimator_config_default(void);
bool ps_estimator_config_is_valid(const ps_estimator_config_t *config);
ps_status_t ps_estimator_init(ps_estimator_t *est,
                              const ps_estimator_config_t *config);
/* `records` must be the manager's current post-expiry snapshot. TTL is owned
 * by the manager; this API intentionally has no second TTL setting. */
ps_status_t ps_estimator_run(const ps_estimator_t *est,
                             const ps_device_record_t **records,
                             size_t count,
                             ps_scanner_health_t bt_health,
                             ps_scanner_health_t wifi_health,
                             const struct timespec *report_time,
                             ps_estimate_t *out);

#endif /* PS_ESTIMATOR_H */
