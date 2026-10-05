/**
 * @file test_observation.c
 * @brief Unit tests for ps_observation_t and ps_types helpers.
 *
 * Phase 0: tests for the data model layer, which is fully defined.
 * These tests do NOT require hardware.
 *
 * Test framework: simple assert-based (no external test library required).
 * Each test function returns 0 on pass, 1 on failure.
 */

#include "ps_observation.h"
#include "ps_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int test_observation_init_zeroes_fields(void)
{
    ps_observation_t obs;
    /* Dirty the memory first to ensure init actually zeroes. */
    memset(&obs, 0xFF, sizeof(obs));

    ps_observation_init(&obs);

    assert(obs.source   == PS_SOURCE_UNKNOWN);
    assert(obs.rssi_dbm == PS_RSSI_UNAVAILABLE);
    /* name should be all zeros (empty string) */
    assert(obs.name[0] == '\0');
    printf("PASS: test_observation_init_zeroes_fields\n");
    return 0;
}

static int test_mac_addr_size(void)
{
    /* The _Static_assert in ps_types.h already guards this at compile time.
     * This runtime check documents the expectation explicitly. */
    assert(sizeof(ps_mac_addr_t) == PS_MAC_STR_LEN);
    printf("PASS: test_mac_addr_size\n");
    return 0;
}

static int test_rssi_sentinel_is_negative(void)
{
    /* PS_RSSI_UNAVAILABLE must be distinct from any real RSSI value.
     * Real RSSI is in range [-127, +20] dBm for BT and similar for Wi-Fi.
     * INT8_MIN = -128, which is below any valid reading. */
    assert(PS_RSSI_UNAVAILABLE < -127);
    printf("PASS: test_rssi_sentinel_is_negative\n");
    return 0;
}

static int test_source_name_strings(void)
{
    assert(strcmp(ps_source_name(PS_SOURCE_BLUETOOTH), "Bluetooth") == 0);
    assert(strcmp(ps_source_name(PS_SOURCE_WIFI),      "Wi-Fi")     == 0);
    assert(strcmp(ps_source_name(PS_SOURCE_UNKNOWN),   "Unknown")   == 0);
    printf("PASS: test_source_name_strings\n");
    return 0;
}

static int test_status_str_ok(void)
{
    assert(strcmp(ps_status_str(PS_OK), "OK") == 0);
    assert(strcmp(ps_status_str(PS_ERR_NO_HARDWARE), "No hardware adapter") == 0);
    assert(strcmp(ps_status_str(PS_ERR_PERMISSION), "Permission denied") == 0);
    printf("PASS: test_status_str_ok\n");
    return 0;
}

int main(void)
{
    int failures = 0;
    printf("=== test_observation ===\n");

    failures += test_observation_init_zeroes_fields();
    failures += test_mac_addr_size();
    failures += test_rssi_sentinel_is_negative();
    failures += test_source_name_strings();
    failures += test_status_str_ok();

    if (failures == 0) {
        printf("All tests PASSED.\n");
    } else {
        printf("%d test(s) FAILED.\n", failures);
    }
    return failures ? 1 : 0;
}
