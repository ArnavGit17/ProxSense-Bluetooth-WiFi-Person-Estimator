/**
 * @file test_bt_scanner.c
 * @brief Unit tests for the Bluetooth HCI parser.
 */

#include "ps_bt_scanner.h"
#include "ps_bt_scanner_priv.h"
#include "ps_obs_manager.h"
#include "ps_observation.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Helper to build a basic HCI LE Advertising Report */
static size_t build_valid_adv_report(uint8_t *buf, uint8_t bdaddr_type, const uint8_t *bdaddr,
                                     const uint8_t *ad_data, size_t ad_len, int8_t rssi)
{
    buf[0] = 0x04; /* HCI_EVENT_PKT */
    buf[1] = 0x3E; /* EVT_LE_META_EVENT */
    
    size_t event_len = 1 + 1 + 1 + 1 + 6 + 1 + ad_len + 1; /* Subevent + num_reports + evt_type + addr_type + MAC + data_len + AD data + RSSI */
    buf[2] = (uint8_t)event_len;
    
    buf[3] = 0x02; /* EVT_LE_ADVERTISING_REPORT */
    buf[4] = 1;    /* Num_reports = 1 */
    buf[5] = 0x00; /* Event_type = ADV_IND */
    buf[6] = bdaddr_type;
    
    /* MAC address (little-endian) */
    memcpy(&buf[7], bdaddr, 6);
    
    buf[13] = (uint8_t)ad_len;
    
    if (ad_len > 0) {
        memcpy(&buf[14], ad_data, ad_len);
    }
    
    buf[14 + ad_len] = (uint8_t)rssi;
    
    return 3 + event_len; /* Total packet length */
}

static int test_valid_single_report_public(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA}; /* AA:BB:CC:DD:EE:01 */
    uint8_t ad_data[] = { 0x0C, 0x09, 'H','e','l','l','o',' ','W','o','r','l','d' };
    
    size_t len = build_valid_adv_report(buf, 0x00, mac, ad_data, sizeof(ad_data), -65);
    
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    assert(rc == PS_OK);
    assert(obs.source == PS_SOURCE_BLUETOOTH);
    assert(strcmp(obs.mac.str, "AA:BB:CC:DD:EE:01") == 0);
    assert(obs.rssi_dbm == -65);
    assert(obs.meta.bt.addr_type == PS_BT_ADDR_PUBLIC);
    assert(strcmp(obs.name, "Hello World") == 0);
    
    printf("PASS: test_valid_single_report_public\n");
    return 0;
}

static int test_valid_single_report_random(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    uint8_t ad_data[] = { 0x05, 0x08, 'T','e','s','t' }; /* Shortened name */
    
    size_t len = build_valid_adv_report(buf, 0x01, mac, ad_data, sizeof(ad_data), -70);
    
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    assert(rc == PS_OK);
    assert(strcmp(obs.mac.str, "AA:BB:CC:DD:EE:FF") == 0);
    assert(obs.rssi_dbm == -70);
    assert(obs.meta.bt.addr_type == PS_BT_ADDR_RANDOM);
    assert(strcmp(obs.name, "Test") == 0);
    
    printf("PASS: test_valid_single_report_random\n");
    return 0;
}

static int test_unavailable_rssi(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    
    /* BlueZ sometimes reports 127 if RSSI is not available */
    size_t len = build_valid_adv_report(buf, 0x00, mac, NULL, 0, 127);
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    assert(rc == PS_OK);
    assert(obs.rssi_dbm == PS_RSSI_UNAVAILABLE);
    
    printf("PASS: test_unavailable_rssi\n");
    return 0;
}

static int test_zero_rssi_is_valid_and_random_address_type(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    size_t len = build_valid_adv_report(buf, 0x01, mac, NULL, 0, 0);

    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);

    assert(rc == PS_OK);
    assert(obs.rssi_dbm == 0);
    assert(obs.meta.bt.addr_type == PS_BT_ADDR_RANDOM);
    assert(strcmp(obs.mac.str, "AA:BB:CC:DD:EE:01") == 0);

    printf("PASS: test_zero_rssi_is_valid_and_random_address_type\n");
    return 0;
}

static int test_malformed_truncated_packet(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x00};
    size_t len = build_valid_adv_report(buf, 0x00, mac, NULL, 0, -60);
    
    ps_observation_t obs;
    
    /* Truncate outer length */
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len - 2, &obs);
    assert(rc == PS_ERR_TIMEOUT);
    
    /* Truncate inner length to be shorter than promised */
    buf[2] = 20; /* Promise 20 bytes payload */
    rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    assert(rc == PS_ERR_TIMEOUT);
    
    printf("PASS: test_malformed_truncated_packet\n");
    return 0;
}

static int test_invalid_event_type(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x00};
    size_t len = build_valid_adv_report(buf, 0x00, mac, NULL, 0, -60);
    
    buf[1] = 0x3F; /* Not EVT_LE_META_EVENT */
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    assert(rc == PS_ERR_TIMEOUT);
    
    printf("PASS: test_invalid_event_type\n");
    return 0;
}

static int test_multiple_ad_structures(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    /* 3 bytes flags, 5 bytes name */
    uint8_t ad_data[] = { 
        0x02, 0x01, 0x06, 
        0x04, 0x09, 'A','B','C',
        0x03, 0xFF, 0x12, 0x34
    };
    
    size_t len = build_valid_adv_report(buf, 0x00, mac, ad_data, sizeof(ad_data), -50);
    
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    assert(rc == PS_OK);
    assert(strcmp(obs.name, "ABC") == 0);
    
    printf("PASS: test_multiple_ad_structures\n");
    return 0;
}

static int test_name_exceeds_buffer(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    uint8_t ad_data[80];
    ad_data[0] = 70; /* Length */
    ad_data[1] = 0x09; /* Complete local name */
    memset(&ad_data[2], 'X', 69);
    
    size_t len = build_valid_adv_report(buf, 0x00, mac, ad_data, 71, -50);
    
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    assert(rc == PS_OK);
    /* Should truncate safely */
    assert(strlen(obs.name) == PS_DEVICE_NAME_MAX_LEN - 1);
    
    printf("PASS: test_name_exceeds_buffer\n");
    return 0;
}

static int test_malformed_ad_length(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    uint8_t ad_data[] = { 0x10, 0x09, 'A','B','C' }; /* Claims 16 bytes, but only provides 3 */
    
    size_t len = build_valid_adv_report(buf, 0x00, mac, ad_data, sizeof(ad_data), -50);
    
    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    
    /* It shouldn't crash, and it shouldn't successfully parse the name */
    assert(rc == PS_OK);
    assert(obs.name[0] == '\0');
    
    printf("PASS: test_malformed_ad_length\n");
    return 0;
}

static int test_multiple_reports(void)
{
    uint8_t buf[256];
    memset(buf, 0, sizeof(buf));

    /* Real HCI LE advertising event with 2 reports in one meta-event. */
    buf[0] = 0x04; /* HCI_EVENT_PKT */
    buf[1] = 0x3E; /* EVT_LE_META_EVENT */
    buf[2] = 24;   /* Parameter total length */
    buf[3] = 0x02; /* EVT_LE_ADVERTISING_REPORT */
    buf[4] = 2;    /* Num_reports = 2 */

    /* Report 1 */
    buf[5] = 0x00; /* ADV_IND */
    buf[6] = 0x00; /* Public address */
    memset(&buf[7], 0x11, 6); /* 11:11:11:11:11:11 */
    buf[13] = 1; /* Data length */
    buf[14] = 0x00; /* Empty AD data */
    buf[15] = (uint8_t)-50; /* RSSI */

    /* Report 2 */
    buf[16] = 0x00; /* ADV_IND */
    buf[17] = 0x01; /* Random address */
    memset(&buf[18], 0x22, 6); /* 22:22:22:22:22:22 */
    buf[24] = 1; /* Data length */
    buf[25] = 0x00; /* Empty AD data */
    buf[26] = (uint8_t)-60; /* RSSI */

    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, 27, &obs);

    assert(rc == PS_OK);
    assert(strcmp(obs.mac.str, "11:11:11:11:11:11") == 0);
    assert(obs.rssi_dbm == -50);
    assert(obs.meta.bt.addr_type == PS_BT_ADDR_PUBLIC);
    assert(obs.name[0] == '\0');

    printf("PASS: test_multiple_reports\n");
    return 0;
}

static int test_bt_parser_to_obs_manager_integration(void)
{
    uint8_t buf[256];
    uint8_t mac[6] = {0x01, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
    uint8_t ad_data[] = {0x05, 0x09, 'P', 'h', 'o', 'n', 'e'};
    size_t len = build_valid_adv_report(buf, 0x01, mac, ad_data, sizeof(ad_data), -42);

    ps_observation_t obs;
    ps_status_t rc = ps_bt_parse_hci_le_adv_report(buf, len, &obs);
    assert(rc == PS_OK);
    assert(obs.source == PS_SOURCE_BLUETOOTH);
    assert(strcmp(obs.mac.str, "AA:BB:CC:DD:EE:01") == 0);
    assert(obs.meta.bt.addr_type == PS_BT_ADDR_RANDOM);
    assert(obs.rssi_dbm == -42);

    ps_obs_manager_t mgr;
    ps_filter_config_t filter = ps_filter_config_default();
    assert(ps_obs_manager_init(&mgr, &filter) == PS_OK);
    assert(ps_obs_manager_process(&mgr, &obs) == PS_OK);
    assert(mgr.table_count == 1);
    assert(mgr.table[0].source == PS_SOURCE_BLUETOOTH);
    assert(strcmp(mgr.table[0].mac.str, "AA:BB:CC:DD:EE:01") == 0);
    assert(mgr.table[0].rssi_last_dbm == -42);

    ps_observation_t dup;
    memcpy(&dup, &obs, sizeof(dup));
    dup.rssi_dbm = -40;
    assert(ps_obs_manager_process(&mgr, &dup) == PS_OK);
    assert(mgr.table_count == 1);
    assert(mgr.table[0].rssi_last_dbm == -40);

    printf("PASS: test_bt_parser_to_obs_manager_integration\n");
    return 0;
}

int main(void)
{
    printf("=== test_bt_scanner ===\n");
    int fail = 0;
    
    fail += test_valid_single_report_public();
    fail += test_valid_single_report_random();
    fail += test_unavailable_rssi();
    fail += test_zero_rssi_is_valid_and_random_address_type();
    fail += test_malformed_truncated_packet();
    fail += test_invalid_event_type();
    fail += test_multiple_ad_structures();
    fail += test_name_exceeds_buffer();
    fail += test_malformed_ad_length();
    fail += test_multiple_reports();
    fail += test_bt_parser_to_obs_manager_integration();
    
    if (fail == 0) {
        printf("All tests PASSED.\n");
    } else {
        printf("%d tests FAILED.\n", fail);
    }
    
    return fail;
}
