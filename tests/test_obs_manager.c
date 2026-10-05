/**
 * @file test_obs_manager.c
 * @brief Unit tests for the Observation Manager — Phase 1.
 *
 * These tests do NOT require hardware. They verify the software logic of
 * filtering, deduplication, TTL expiry, and snapshot.
 *
 * All MAC addresses used here are made-up test values. They do not
 * represent real devices or real hardware scans.
 *
 * Test coverage:
 *   A. Initialisation (null filter, custom filter, null pointer)
 *   B. Duplicate observation handling (same MAC seen twice)
 *   C. RSSI filter (below threshold, at threshold, unavailable)
 *   D. Observation update (RSSI, sighting count, name)
 *   E. New vs duplicate insertion
 *   F. Table overflow / LRU eviction
 *   G. TTL expiry (empty table, no expiry due, force-aging)
 *   H. Snapshot (count, pointer validity)
 *   I. Multi-source independence (BT and Wi-Fi same MAC = distinct records)
 *   J. NULL argument handling on all public functions
 */

#include "ps_obs_manager.h"
#include "ps_observation.h"
#include "ps_types.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Test helpers
 * ========================================================================= */

/** Build a minimal BT observation with the given MAC and RSSI. */
static ps_observation_t make_bt_obs(const char *mac_str, int8_t rssi)
{
    ps_observation_t obs;
    ps_observation_init(&obs);
    obs.source    = PS_SOURCE_BLUETOOTH;
    obs.rssi_dbm  = rssi;
    strncpy(obs.mac.str, mac_str, PS_MAC_STR_LEN - 1);
    obs.mac.str[PS_MAC_STR_LEN - 1] = '\0';
    obs.meta.bt.addr_type = PS_BT_ADDR_PUBLIC;
    return obs;
}

/** Build a minimal Wi-Fi observation (probe request type). */
static ps_observation_t make_wifi_obs(const char *mac_str, int8_t rssi)
{
    ps_observation_t obs;
    ps_observation_init(&obs);
    obs.source    = PS_SOURCE_WIFI;
    obs.rssi_dbm  = rssi;
    strncpy(obs.mac.str, mac_str, PS_MAC_STR_LEN - 1);
    obs.mac.str[PS_MAC_STR_LEN - 1] = '\0';
    obs.meta.wifi.frame_type = PS_WIFI_FRAME_PROBE_REQ;
    return obs;
}

/** Build a BT observation with a random (rotating) address type. */
static ps_observation_t make_bt_obs_random(const char *mac_str, int8_t rssi)
{
    ps_observation_t obs = make_bt_obs(mac_str, rssi);
    obs.meta.bt.addr_type = PS_BT_ADDR_RANDOM;
    return obs;
}

/* =========================================================================
 * A. Initialisation tests
 * ========================================================================= */

static int test_init_null_filter(void)
{
    ps_obs_manager_t mgr;
    ps_status_t s = ps_obs_manager_init(&mgr, NULL);
    assert(s == PS_OK);
    assert(mgr.table_count == 0);
    assert(mgr.filter.rssi_min_dbm < 0);
    assert(mgr.filter.device_ttl_s > 0);
    printf("PASS: test_init_null_filter\n");
    return 0;
}

static int test_init_custom_filter(void)
{
    ps_filter_config_t f = {
        .rssi_min_dbm     = -70,
        .device_ttl_s     = 30,
        .min_duration_s   = 10,
        .filter_random_bt = false
    };
    ps_obs_manager_t mgr;
    ps_status_t s = ps_obs_manager_init(&mgr, &f);
    assert(s == PS_OK);
    assert(mgr.filter.rssi_min_dbm == -70);
    assert(mgr.filter.device_ttl_s == 30);
    assert(mgr.filter.min_duration_s == 10);
    assert(mgr.filter.filter_random_bt == false);
    printf("PASS: test_init_custom_filter\n");
    return 0;
}

static int test_init_null_pointer(void)
{
    ps_status_t s = ps_obs_manager_init(NULL, NULL);
    assert(s == PS_ERR_INVALID_ARG);
    printf("PASS: test_init_null_pointer\n");
    return 0;
}

/* =========================================================================
 * B. Duplicate handling
 * ========================================================================= */

static int test_new_device_inserted(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:01", -60);
    ps_status_t s = ps_obs_manager_process(&mgr, &obs);
    assert(s == PS_OK);
    assert(mgr.table_count == 1);
    assert(mgr.table[0].sighting_count == 1);
    printf("PASS: test_new_device_inserted\n");
    return 0;
}

static int test_duplicate_updates_record(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs1 = make_bt_obs("AA:BB:CC:DD:EE:02", -60);
    ps_observation_t obs2 = make_bt_obs("AA:BB:CC:DD:EE:02", -55);

    ps_obs_manager_process(&mgr, &obs1);
    ps_obs_manager_process(&mgr, &obs2);

    /* Still only one record */
    assert(mgr.table_count == 1);
    assert(mgr.table[0].sighting_count == 2);
    /* RSSI should be updated to latest */
    assert(mgr.table[0].rssi_last_dbm == -55);
    /* Peak RSSI is the strongest (highest dBm value = -55 here) */
    assert(mgr.table[0].rssi_peak_dbm == -55);
    printf("PASS: test_duplicate_updates_record\n");
    return 0;
}

static int test_two_different_macs_are_two_records(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs1 = make_bt_obs("AA:BB:CC:DD:EE:03", -60);
    ps_observation_t obs2 = make_bt_obs("AA:BB:CC:DD:EE:04", -62);

    ps_obs_manager_process(&mgr, &obs1);
    ps_obs_manager_process(&mgr, &obs2);

    assert(mgr.table_count == 2);
    printf("PASS: test_two_different_macs_are_two_records\n");
    return 0;
}

/* =========================================================================
 * C. RSSI filtering
 * ========================================================================= */

static int test_rssi_below_threshold_filtered(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.rssi_min_dbm = -75; /* threshold */

    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);

    /* RSSI is -80, below threshold -75 → should be filtered out */
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:05", -80);
    ps_status_t s = ps_obs_manager_process(&mgr, &obs);
    assert(s == PS_OK);           /* Not an error — just filtered */
    assert(mgr.table_count == 0); /* Nothing inserted */
    printf("PASS: test_rssi_below_threshold_filtered\n");
    return 0;
}

static int test_rssi_at_threshold_accepted(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.rssi_min_dbm = -75;

    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);

    /* Exactly at threshold — should be accepted */
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:06", -75);
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table_count == 1);
    printf("PASS: test_rssi_at_threshold_accepted\n");
    return 0;
}

static int test_rssi_unavailable_accepted(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    /* RSSI unavailable — must always be accepted */
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:07", PS_RSSI_UNAVAILABLE);
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table_count == 1);
    printf("PASS: test_rssi_unavailable_accepted\n");
    return 0;
}

/* =========================================================================
 * D. Field updates on re-sighting
 * ========================================================================= */

static int test_peak_rssi_tracks_strongest(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:08", -70);
    ps_obs_manager_process(&mgr, &obs);

    /* Weaker reading — peak should not decrease */
    obs.rssi_dbm = -80;
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table[0].rssi_peak_dbm == -70);

    /* Stronger reading — peak should update */
    obs.rssi_dbm = -50;
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table[0].rssi_peak_dbm == -50);

    printf("PASS: test_peak_rssi_tracks_strongest\n");
    return 0;
}

static int test_name_set_on_first_sighting_with_name(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:09", -60);
    strncpy(obs.name, "TestPhone", PS_DEVICE_NAME_MAX_LEN - 1);
    obs.name[PS_DEVICE_NAME_MAX_LEN - 1] = '\0';

    ps_obs_manager_process(&mgr, &obs);
    assert(strcmp(mgr.table[0].name, "TestPhone") == 0);
    printf("PASS: test_name_set_on_first_sighting_with_name\n");
    return 0;
}

static int test_name_not_overwritten_once_set(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:0A", -60);
    strncpy(obs.name, "Original", PS_DEVICE_NAME_MAX_LEN - 1);
    ps_obs_manager_process(&mgr, &obs);

    /* Re-sight without a name — name should remain */
    ps_observation_t obs2 = make_bt_obs("AA:BB:CC:DD:EE:0A", -62);
    /* name is empty (ps_observation_init zeroes it) */
    ps_obs_manager_process(&mgr, &obs2);

    assert(strcmp(mgr.table[0].name, "Original") == 0);
    printf("PASS: test_name_not_overwritten_once_set\n");
    return 0;
}

/* =========================================================================
 * E. BLE random address flag
 * ========================================================================= */

static int test_random_bt_flag_set(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs_random("AA:BB:CC:DD:EE:0B", -60);
    ps_obs_manager_process(&mgr, &obs);

    assert(mgr.table_count == 1);
    assert(mgr.table[0].is_random_bt == true);
    printf("PASS: test_random_bt_flag_set\n");
    return 0;
}

static int test_public_bt_flag_not_set(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:0C", -60);
    obs.meta.bt.addr_type = PS_BT_ADDR_PUBLIC;
    ps_obs_manager_process(&mgr, &obs);

    assert(mgr.table[0].is_random_bt == false);
    printf("PASS: test_public_bt_flag_not_set\n");
    return 0;
}

/* =========================================================================
 * F. Multi-source independence
 * ========================================================================= */

static int test_bt_and_wifi_same_mac_are_separate(void)
{
    /*
     * This test documents a critical design invariant:
     * A BT MAC and a Wi-Fi MAC that happen to be identical strings are
     * treated as DIFFERENT devices because they come from different radios.
     * We cannot assume the same MAC string on both radios is the same device;
     * MAC randomisation makes this correlation unreliable.
     */
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    const char *shared_mac = "AA:BB:CC:DD:EE:0D";
    ps_observation_t bt_obs   = make_bt_obs(shared_mac, -60);
    ps_observation_t wifi_obs = make_wifi_obs(shared_mac, -65);

    ps_obs_manager_process(&mgr, &bt_obs);
    ps_obs_manager_process(&mgr, &wifi_obs);

    /* Must be TWO separate records — different radios */
    assert(mgr.table_count == 2);
    printf("PASS: test_bt_and_wifi_same_mac_are_separate\n");
    return 0;
}

/* =========================================================================
 * G. Expiry / TTL
 * ========================================================================= */

static int test_expire_empty_table(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);
    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 0);
    printf("PASS: test_expire_empty_table\n");
    return 0;
}

static int test_expire_fresh_entries_not_removed(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:0E", -60);
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table_count == 1);

    /* Expire immediately — device was just seen, TTL default is 60s */
    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 0);
    assert(mgr.table_count == 1);
    printf("PASS: test_expire_fresh_entries_not_removed\n");
    return 0;
}

static int test_expire_aged_entry_removed(void)
{
    /*
     * We cannot easily sleep 60 s in a unit test.
     * Instead we manipulate the record's last_seen timestamp directly
     * to simulate an old entry. This tests the expiry logic itself
     * without a real time dependency.
     */
    ps_filter_config_t f = ps_filter_config_default();
    f.device_ttl_s = 5; /* short TTL for test */

    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:0F", -60);
    ps_obs_manager_process(&mgr, &obs);
    assert(mgr.table_count == 1);

    /* Wind back last_seen by 10 seconds (past TTL of 5). */
    mgr.table[0].last_seen.tv_sec -= 10;

    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 1);
    assert(mgr.table_count == 0);
    printf("PASS: test_expire_aged_entry_removed\n");
    return 0;
}

static int test_expire_only_old_entries_removed(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.device_ttl_s = 5;

    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);

    ps_observation_t old = make_bt_obs("AA:BB:CC:DD:EE:10", -60);
    ps_observation_t fresh = make_bt_obs("AA:BB:CC:DD:EE:11", -62);

    ps_obs_manager_process(&mgr, &old);
    ps_obs_manager_process(&mgr, &fresh);
    assert(mgr.table_count == 2);

    /* Age only the first entry */
    mgr.table[0].last_seen.tv_sec -= 10;

    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 1);
    assert(mgr.table_count == 1);
    printf("PASS: test_expire_only_old_entries_removed\n");
    return 0;
}

static int test_expire_exactly_at_ttl(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.device_ttl_s = 5;
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:10", -60);
    ps_obs_manager_process(&mgr, &obs);
    mgr.table[0].last_seen.tv_sec -= 5; /* Exactly at TTL */
    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 1);
    assert(mgr.table_count == 0);
    printf("PASS: test_expire_exactly_at_ttl\n");
    return 0;
}

static int test_expire_just_before_ttl(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.device_ttl_s = 5;
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:10", -60);
    ps_obs_manager_process(&mgr, &obs);
    mgr.table[0].last_seen.tv_sec -= 4; /* Safely before TTL (4 seconds ago) */
    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 0);
    assert(mgr.table_count == 1);
    printf("PASS: test_expire_just_before_ttl\n");
    return 0;
}

static int test_expire_just_after_ttl(void)
{
    ps_filter_config_t f = ps_filter_config_default();
    f.device_ttl_s = 5;
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, &f);
    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:10", -60);
    ps_obs_manager_process(&mgr, &obs);
    mgr.table[0].last_seen.tv_sec -= 6; /* Safely after TTL (6 seconds ago) */
    size_t expired = ps_obs_manager_expire(&mgr);
    assert(expired == 1);
    assert(mgr.table_count == 0);
    printf("PASS: test_expire_just_after_ttl\n");
    return 0;
}

/* =========================================================================
 * G2. Table Overflow (LRU)
 * ========================================================================= */
static int test_table_overflow_lru_eviction(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    /* Fill the table to maximum capacity */
    for (size_t i = 0; i < PS_OBS_MANAGER_MAX_DEVICES; ++i) {
        char mac[PS_MAC_STR_LEN];
        /* generate unique MACs like 00:00:00:00:00:00 to 00:00:00:00:01:FF */
        snprintf(mac, sizeof(mac), "00:00:00:00:%02X:%02X", (unsigned int)(i >> 8), (unsigned int)(i & 0xFF));
        ps_observation_t obs = make_bt_obs(mac, -60);
        ps_obs_manager_process(&mgr, &obs);
        /* Set last_seen sequentially so entry 0 is the oldest */
        mgr.table[i].last_seen.tv_sec = (time_t)i;
        mgr.table[i].last_seen.tv_nsec = 0;
    }
    
    assert(mgr.table_count == PS_OBS_MANAGER_MAX_DEVICES);
    /* Save the mac of the oldest entry (i=0) */
    char oldest_mac[PS_MAC_STR_LEN];
    strncpy(oldest_mac, mgr.table[0].mac.str, PS_MAC_STR_LEN);
    
    /* Save the mac of a newer entry (i=1) */
    char newer_mac[PS_MAC_STR_LEN];
    strncpy(newer_mac, mgr.table[1].mac.str, PS_MAC_STR_LEN);

    /* Insert one more device, which should trigger eviction of oldest_mac */
    ps_observation_t overflow_obs = make_bt_obs("FF:FF:FF:FF:FF:FF", -50);
    ps_status_t s = ps_obs_manager_process(&mgr, &overflow_obs);
    
    /* Status should be PS_ERR_FULL, but the device should be inserted anyway */
    assert(s == PS_ERR_FULL);
    assert(mgr.table_count == PS_OBS_MANAGER_MAX_DEVICES);
    
    /* Verify the oldest MAC was removed, and newer MAC is still present */
    bool oldest_found = false;
    bool newer_found = false;
    bool overflow_found = false;
    for (size_t i = 0; i < mgr.table_count; ++i) {
        if (strcmp(mgr.table[i].mac.str, oldest_mac) == 0) oldest_found = true;
        if (strcmp(mgr.table[i].mac.str, newer_mac) == 0) newer_found = true;
        if (strcmp(mgr.table[i].mac.str, "FF:FF:FF:FF:FF:FF") == 0) overflow_found = true;
    }
    assert(oldest_found == false);
    assert(newer_found == true);
    assert(overflow_found == true);

    printf("PASS: test_table_overflow_lru_eviction\n");
    return 0;
}

/* =========================================================================
 * H. Snapshot
 * ========================================================================= */

static int test_snapshot_empty(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    const ps_device_record_t *snap[10];
    size_t count = 99; /* sentinel */
    ps_status_t s = ps_obs_manager_snapshot(&mgr, snap, 10, &count);
    assert(s == PS_OK);
    assert(count == 0);
    printf("PASS: test_snapshot_empty\n");
    return 0;
}

static int test_snapshot_returns_correct_count(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t o1 = make_bt_obs("AA:BB:CC:DD:EE:12", -60);
    ps_observation_t o2 = make_bt_obs("AA:BB:CC:DD:EE:13", -62);
    ps_obs_manager_process(&mgr, &o1);
    ps_obs_manager_process(&mgr, &o2);

    const ps_device_record_t *snap[10];
    size_t count = 0;
    ps_obs_manager_snapshot(&mgr, snap, 10, &count);
    assert(count == 2);
    assert(snap[0] != NULL);
    assert(snap[1] != NULL);
    printf("PASS: test_snapshot_returns_correct_count\n");
    return 0;
}

static int test_snapshot_respects_max_records(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    for (int i = 0; i < 5; ++i) {
        char mac[PS_MAC_STR_LEN];
        snprintf(mac, sizeof(mac), "AA:BB:CC:DD:EE:%02X", i + 0x20);
        ps_observation_t obs = make_bt_obs(mac, -60);
        ps_obs_manager_process(&mgr, &obs);
    }
    assert(mgr.table_count == 5);

    /* Request only 3 */
    const ps_device_record_t *snap[3];
    size_t count = 0;
    ps_obs_manager_snapshot(&mgr, snap, 3, &count);
    assert(count == 3);
    printf("PASS: test_snapshot_respects_max_records\n");
    return 0;
}

static int test_snapshot_null_args(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    const ps_device_record_t *snap[10];
    size_t count = 0;

    assert(ps_obs_manager_snapshot(NULL, snap, 10, &count) == PS_ERR_INVALID_ARG);
    assert(ps_obs_manager_snapshot(&mgr, NULL, 10, &count) == PS_ERR_INVALID_ARG);
    assert(ps_obs_manager_snapshot(&mgr, snap, 10, NULL)   == PS_ERR_INVALID_ARG);
    printf("PASS: test_snapshot_null_args\n");
    return 0;
}

/* =========================================================================
 * I. NULL / invalid argument handling
 * ========================================================================= */

static int test_process_null_args(void)
{
    ps_obs_manager_t mgr;
    ps_obs_manager_init(&mgr, NULL);

    ps_observation_t obs = make_bt_obs("AA:BB:CC:DD:EE:14", -60);
    assert(ps_obs_manager_process(NULL, &obs) == PS_ERR_INVALID_ARG);
    assert(ps_obs_manager_process(&mgr, NULL) == PS_ERR_INVALID_ARG);
    printf("PASS: test_process_null_args\n");
    return 0;
}

static int test_expire_null_returns_zero(void)
{
    size_t n = ps_obs_manager_expire(NULL);
    assert(n == 0);
    printf("PASS: test_expire_null_returns_zero\n");
    return 0;
}

/* =========================================================================
 * Main test runner
 * ========================================================================= */

int main(void)
{
    int failures = 0;
    printf("=== test_obs_manager ===\n");

    /* A. Initialisation */
    failures += test_init_null_filter();
    failures += test_init_custom_filter();
    failures += test_init_null_pointer();

    /* B. New / duplicate */
    failures += test_new_device_inserted();
    failures += test_duplicate_updates_record();
    failures += test_two_different_macs_are_two_records();

    /* C. RSSI filtering */
    failures += test_rssi_below_threshold_filtered();
    failures += test_rssi_at_threshold_accepted();
    failures += test_rssi_unavailable_accepted();

    /* D. Field updates */
    failures += test_peak_rssi_tracks_strongest();
    failures += test_name_set_on_first_sighting_with_name();
    failures += test_name_not_overwritten_once_set();

    /* E. BLE random address */
    failures += test_random_bt_flag_set();
    failures += test_public_bt_flag_not_set();

    /* F. Multi-source independence */
    failures += test_bt_and_wifi_same_mac_are_separate();

    /* G. Expiry */
    failures += test_expire_empty_table();
    failures += test_expire_fresh_entries_not_removed();
    failures += test_expire_aged_entry_removed();
    failures += test_expire_only_old_entries_removed();
    failures += test_expire_exactly_at_ttl();
    failures += test_expire_just_before_ttl();
    failures += test_expire_just_after_ttl();

    /* G2. Overflow */
    failures += test_table_overflow_lru_eviction();

    /* H. Snapshot */
    failures += test_snapshot_empty();
    failures += test_snapshot_returns_correct_count();
    failures += test_snapshot_respects_max_records();
    failures += test_snapshot_null_args();

    /* I. NULL/invalid args */
    failures += test_process_null_args();
    failures += test_expire_null_returns_zero();

    if (failures == 0) {
        printf("All tests PASSED.\n");
    } else {
        printf("%d test(s) FAILED.\n", failures);
    }
    return failures ? 1 : 0;
}
