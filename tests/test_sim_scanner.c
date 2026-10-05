/**
 * @file test_sim_scanner.c
 * @brief Unit tests for the simulation scanner — Phase 1.
 *
 * Tests the complete simulation scanner lifecycle and scenario parsing.
 * All tests are deterministic — no real hardware, no sleeping (realtime=false).
 *
 * Test coverage:
 *   A. Init with NULL config (uses defaults)
 *   B. Init with NULL scanner pointer (must fail)
 *   C. Open with built-in scenario (no file)
 *   D. Open with scenario file (scenario_2_people.txt)
 *   E. read_next returns PS_OK then PS_ERR_TIMEOUT at end-of-data
 *   F. Observations from scenario file have correct source, MAC, RSSI
 *   G. read_next before open returns error
 *   H. describe() returns a non-NULL non-empty string
 *   I. close() is safe to call twice
 *   J. Simulation scanner feeds obs_manager correctly
 */

#include "ps_sim_scanner.h"
#include "ps_obs_manager.h"
#include "ps_scanner.h"
#include "ps_observation.h"
#include "ps_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Path to the standard scenario file (relative to where tests are run) */
#define SCENARIO_FILE "tests/scenario_2_people.txt"

/* =========================================================================
 * A. Init with NULL config — uses defaults
 * ========================================================================= */

static int test_init_null_config(void)
{
    ps_scanner_t scanner;
    ps_status_t s = ps_sim_scanner_init(&scanner, NULL);
    assert(s == PS_OK);
    assert(scanner.ops  != NULL);
    assert(scanner.priv != NULL);
    assert(scanner.is_open == false);

    ps_scanner_close(&scanner); /* clean up */
    printf("PASS: test_init_null_config\n");
    return 0;
}

/* =========================================================================
 * B. Init with NULL scanner pointer
 * ========================================================================= */

static int test_init_null_scanner(void)
{
    ps_status_t s = ps_sim_scanner_init(NULL, NULL);
    assert(s == PS_ERR_INVALID_ARG);
    printf("PASS: test_init_null_scanner\n");
    return 0;
}

/* =========================================================================
 * C. Open with built-in scenario
 * ========================================================================= */

static int test_open_builtin_scenario(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = NULL,  /* Use built-in */
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);

    ps_status_t s = ps_scanner_open(&scanner);
    assert(s == PS_OK);
    assert(scanner.is_open == true);

    ps_scanner_close(&scanner);
    printf("PASS: test_open_builtin_scenario\n");
    return 0;
}

/* =========================================================================
 * D. Open with scenario file
 * ========================================================================= */

static int test_open_scenario_file(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = SCENARIO_FILE,
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);

    ps_status_t s = ps_scanner_open(&scanner);
    assert(s == PS_OK);

    ps_scanner_close(&scanner);
    printf("PASS: test_open_scenario_file\n");
    return 0;
}

/* =========================================================================
 * D2. Open with missing scenario file fails
 * ========================================================================= */

static int test_open_missing_file_fails(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = "nonexistent_scenario_12345.txt",
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);

    ps_status_t s = ps_scanner_open(&scanner);
    assert(s == PS_ERR_IO);

    ps_scanner_close(&scanner);
    printf("PASS: test_open_missing_file_fails\n");
    return 0;
}

/* =========================================================================
 * E. read_next returns PS_ERR_TIMEOUT when scenario is exhausted
 * ========================================================================= */

static int test_builtin_scenario_exhausts(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = NULL,
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);
    ps_scanner_open(&scanner);

    ps_observation_t obs;
    ps_status_t rc;
    int count = 0;

    /* Read until we get something other than PS_OK */
    while ((rc = ps_scanner_read_next(&scanner, &obs, 0)) == PS_OK) {
        count++;
        /* Safety limit — if the scanner were infinite, this would catch it */
        assert(count < 1000 && "Simulation scanner appears infinite");
    }

    /* The only acceptable terminal condition is PS_ERR_TIMEOUT (end-of-data) */
    assert(rc == PS_ERR_TIMEOUT);
    assert(count > 0); /* Must have delivered at least one observation */

    ps_scanner_close(&scanner);
    printf("PASS: test_builtin_scenario_exhausts (delivered %d obs)\n", count);
    return 0;
}

/* =========================================================================
 * F. Observations from the scenario file have correct fields
 * ========================================================================= */

static int test_scenario_file_obs_correct(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = SCENARIO_FILE,
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);
    ps_scanner_open(&scanner);

    ps_observation_t obs;
    ps_status_t rc = ps_scanner_read_next(&scanner, &obs, 0);

    assert(rc == PS_OK);
    /* First line in scenario_2_people.txt: BT, AA:BB:CC:DD:EE:01, -62 */
    assert(obs.source == PS_SOURCE_BLUETOOTH);
    assert(strcmp(obs.mac.str, "AA:BB:CC:DD:EE:01") == 0);
    assert(obs.rssi_dbm == -62);
    /* Name field should be "Alice-Phone" */
    assert(strcmp(obs.name, "Alice-Phone") == 0);

    ps_scanner_close(&scanner);
    printf("PASS: test_scenario_file_obs_correct\n");
    return 0;
}

/* =========================================================================
 * G. read_next before open() returns an error
 * ========================================================================= */

static int test_read_before_open_fails(void)
{
    ps_sim_scanner_config_t cfg = {
        .scenario_file = NULL,
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);
    /* Do NOT call open() */

    ps_observation_t obs;
    ps_status_t rc = ps_scanner_read_next(&scanner, &obs, 0);
    /* Should not be PS_OK — scanner is not open */
    assert(rc != PS_OK);

    ps_scanner_close(&scanner);
    printf("PASS: test_read_before_open_fails\n");
    return 0;
}

/* =========================================================================
 * H. describe() returns a non-NULL non-empty string
 * ========================================================================= */

static int test_describe_non_null(void)
{
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, NULL);

    const char *desc = ps_scanner_describe(&scanner);
    assert(desc != NULL);
    assert(desc[0] != '\0');

    ps_scanner_close(&scanner);
    printf("PASS: test_describe_non_null\n");
    return 0;
}

/* =========================================================================
 * I. close() is safe to call on an already-closed scanner
 * ========================================================================= */

static int test_close_idempotent(void)
{
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, NULL);
    ps_scanner_open(&scanner);
    ps_scanner_close(&scanner);
    ps_scanner_close(&scanner); /* Second close — must not crash */
    printf("PASS: test_close_idempotent\n");
    return 0;
}

/* =========================================================================
 * J. Simulation scanner feeds obs_manager correctly
 * ========================================================================= */

static int test_sim_feeds_obs_manager(void)
{
    /*
     * This is an integration test for the scanner → obs_manager path.
     * We replay the built-in scenario and count how many devices end up
     * in the observation manager's table.
     */
    ps_sim_scanner_config_t cfg = {
        .scenario_file = NULL,
        .realtime      = false
    };
    ps_scanner_t scanner;
    ps_sim_scanner_init(&scanner, &cfg);
    ps_scanner_open(&scanner);

    ps_filter_config_t f = ps_filter_config_default();
    /* Disable min_duration_s so all observations count immediately. */
    f.min_duration_s = 0;
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);

    ps_observation_t obs;
    while (ps_scanner_read_next(&scanner, &obs, 0) == PS_OK) {
        ps_obs_manager_process(&mgr, &obs);
    }

    /*
     * The built-in scenario has 2 distinct MACs:
     *   AA:BB:CC:DD:EE:01 (BT)
     *   11:22:33:44:55:01 (Wi-Fi)
     * After deduplication we expect exactly 2 records.
     */
    assert(mgr.table_count == 2);

    ps_scanner_close(&scanner);
    printf("PASS: test_sim_feeds_obs_manager\n");
    return 0;
}

/* =========================================================================
 * Main
 * ========================================================================= */

int main(void)
{
    int failures = 0;
    printf("=== test_sim_scanner ===\n");

    failures += test_init_null_config();
    failures += test_init_null_scanner();
    failures += test_open_builtin_scenario();
    failures += test_open_scenario_file();
    failures += test_open_missing_file_fails();
    failures += test_builtin_scenario_exhausts();
    failures += test_scenario_file_obs_correct();
    failures += test_read_before_open_fails();
    failures += test_describe_non_null();
    failures += test_close_idempotent();
    failures += test_sim_feeds_obs_manager();

    if (failures == 0) {
        printf("All tests PASSED.\n");
    } else {
        printf("%d test(s) FAILED.\n", failures);
    }
    return failures ? 1 : 0;
}
