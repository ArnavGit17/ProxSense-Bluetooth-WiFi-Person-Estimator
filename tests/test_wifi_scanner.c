#include "ps_obs_manager.h"
#include "ps_wifi_scanner.h"
#include "ps_wifi_scanner_priv.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_PACKET_CAPACITY 256U

static size_t build_probe(uint8_t *packet,
                          const uint8_t source[6],
                          bool include_rssi,
                          int8_t rssi,
                          bool include_channel)
{
    memset(packet, 0, TEST_PACKET_CAPACITY);
    uint32_t present = 0U;
    if (include_channel) present |= 1U << 3;
    if (include_rssi) present |= 1U << 5;
    packet[0] = 0U;
    packet[1] = 0U;
    packet[4] = (uint8_t)present;
    packet[5] = (uint8_t)(present >> 8);
    packet[6] = (uint8_t)(present >> 16);
    packet[7] = (uint8_t)(present >> 24);

    size_t radiotap_len = 8U;
    if (include_channel) {
        packet[8] = 0x6c; /* 2412 MHz, channel 1 */
        packet[9] = 0x09;
        packet[10] = 0xa0;
        packet[11] = 0x00;
        radiotap_len = 12U;
    }
    if (include_rssi) {
        packet[radiotap_len] = (uint8_t)rssi;
        radiotap_len++;
    }
    packet[2] = (uint8_t)radiotap_len;
    packet[3] = 0U;

    uint8_t *frame = packet + radiotap_len;
    frame[0] = 0x40; /* Management, Probe Request subtype */
    frame[1] = 0x00;
    memset(frame + 4U, 0xff, 6U); /* Destination */
    memcpy(frame + 10U, source, 6U); /* Transmitter/source address */
    memset(frame + 16U, 0, 6U); /* BSSID */
    frame[24] = 0U; /* SSID information element */
    frame[25] = 0U; /* Wildcard SSID */
    return radiotap_len + 26U;
}

static int test_valid_probe_request_with_rssi(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
    size_t length = build_probe(packet, source, true, -48, true);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.source == PS_SOURCE_WIFI);
    assert(strcmp(observation.mac.str, "00:11:22:33:44:55") == 0);
    assert(observation.rssi_dbm == -48);
    assert(observation.meta.wifi.frame_type == PS_WIFI_FRAME_PROBE_REQ);
    assert(observation.meta.wifi.channel == 1U);
    printf("PASS: test_valid_probe_request_with_rssi\n");
    return 0;
}

static int test_valid_probe_request_without_rssi(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.rssi_dbm == PS_RSSI_UNAVAILABLE);
    assert(observation.meta.wifi.channel == 0U);
    printf("PASS: test_valid_probe_request_without_rssi\n");
    return 0;
}

static int test_valid_management_source_extraction(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    size_t length = build_probe(packet, source, true, -60, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(strcmp(observation.mac.str, "02:AA:BB:CC:DD:EE") == 0);
    assert(observation.meta.wifi.locally_administered);
    printf("PASS: test_valid_management_source_extraction\n");
    return 0;
}

static int test_valid_variable_information_elements(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x66};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[33] = 3U;
    memcpy(packet + 34U, "abc", 3U);
    packet[37] = 1U;
    packet[38] = 1U;
    packet[39] = 6U;
    length += 6U;
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    printf("PASS: test_valid_variable_information_elements\n");
    return 0;
}

static int test_truncated_radiotap_header(void)
{
    uint8_t packet[8] = {0};
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, 7U, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_truncated_radiotap_header\n");
    return 0;
}

static int test_invalid_radiotap_lengths(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    packet[2] = 7U;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    packet[2] = 250U;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_invalid_radiotap_lengths\n");
    return 0;
}

static int test_truncated_80211_header(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length - 3U, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_truncated_80211_header\n");
    return 0;
}

static int test_invalid_frame_length(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, true, -55, false);
    ps_observation_t observation;
    packet[2] = 40U;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_invalid_frame_length\n");
    return 0;
}

static int test_non_management_frame(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[8] = 0x08; /* Data frame */
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_UNSUPPORTED);
    printf("PASS: test_non_management_frame\n");
    return 0;
}

static int test_management_frame_not_probe_request(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[8] = 0x80; /* Beacon subtype */
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_UNSUPPORTED);
    printf("PASS: test_management_frame_not_probe_request\n");
    return 0;
}

static int test_truncated_probe_request(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[length - 1U] = 1U; /* IE length without its payload */
    packet[length - 2U] = 0U;
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_truncated_probe_request\n");
    return 0;
}

static int test_malformed_information_element(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[length - 1U] = 5U; /* SSID IE claims bytes not captured */
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_malformed_information_element\n");
    return 0;
}

static int test_randomized_address_hint(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x06, 0x10, 0x20, 0x30, 0x40, 0x50};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.meta.wifi.locally_administered);
    printf("PASS: test_randomized_address_hint\n");
    return 0;
}

static int test_broadcast_source_rejected(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_broadcast_source_rejected\n");
    return 0;
}

static int test_invalid_multicast_source_rejected(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x01, 0x10, 0x20, 0x30, 0x40, 0x50};
    size_t length = build_probe(packet, source, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_invalid_multicast_source_rejected\n");
    return 0;
}

static int test_rssi_edge_cases(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    ps_observation_t observation;
    size_t length = build_probe(packet, source, true, 0, false);
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.rssi_dbm == 0);
    length = build_probe(packet, source, true, 127, false);
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.rssi_dbm == PS_RSSI_UNAVAILABLE);
    length = build_probe(packet, source, true, -127, false);
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    assert(observation.rssi_dbm == -127);
    printf("PASS: test_rssi_edge_cases\n");
    return 0;
}

static int test_extended_radiotap_present_bitmap(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    packet[7] = 0x80U;
    memmove(packet + 12U, packet + 8U, length - 8U);
    packet[4] = 0U;
    packet[8] = 0U;
    packet[9] = 0U;
    packet[10] = 0U;
    packet[11] = 0U;
    packet[2] = 12U;
    length += 4U;
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    printf("PASS: test_extended_radiotap_present_bitmap\n");
    return 0;
}

static int test_unsupported_radiotap_field_is_skipped_safely(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 1, 2, 3, 4, 5};
    size_t length = build_probe(packet, source, false, 0, false);
    memmove(packet + 9U, packet + 8U, length - 8U);
    packet[4] = 0x40U; /* Radiotap field 6 (antenna index), not consumed. */
    packet[2] = 9U;
    packet[8] = 1U;
    length++;
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);
    printf("PASS: test_unsupported_radiotap_field_is_skipped_safely\n");
    return 0;
}

static int test_multiple_captured_frames(void)
{
    uint8_t first[TEST_PACKET_CAPACITY];
    uint8_t second[TEST_PACKET_CAPACITY];
    const uint8_t mac_a[6] = {0x00, 1, 2, 3, 4, 5};
    const uint8_t mac_b[6] = {0x00, 6, 7, 8, 9, 10};
    size_t first_len = build_probe(first, mac_a, false, 0, false);
    size_t second_len = build_probe(second, mac_b, false, 0, false);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(first, first_len, &observation) == PS_OK);
    assert(strcmp(observation.mac.str, "00:01:02:03:04:05") == 0);
    assert(ps_wifi_parse_radiotap_probe_request(second, second_len, &observation) == PS_OK);
    assert(strcmp(observation.mac.str, "00:06:07:08:09:0A") == 0);
    printf("PASS: test_multiple_captured_frames\n");
    return 0;
}

static int test_empty_capture_buffer(void)
{
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(NULL, 0U, &observation) == PS_ERR_INVALID_ARG);
    uint8_t packet[1] = {0U};
    assert(ps_wifi_parse_radiotap_probe_request(packet, 0U, &observation) == PS_ERR_MALFORMED);
    printf("PASS: test_empty_capture_buffer\n");
    return 0;
}

static int test_wifi_parser_to_observation_manager(void)
{
    uint8_t packet[TEST_PACKET_CAPACITY];
    const uint8_t source[6] = {0x00, 0xaa, 0xbb, 0xcc, 0xdd, 0xee};
    size_t length = build_probe(packet, source, true, -42, true);
    ps_observation_t observation;
    assert(ps_wifi_parse_radiotap_probe_request(packet, length, &observation) == PS_OK);

    ps_obs_manager_t manager;
    ps_filter_config_t filter = ps_filter_config_default();
    assert(ps_obs_manager_init(&manager, &filter) == PS_OK);
    assert(ps_obs_manager_process(&manager, &observation) == PS_OK);
    assert(manager.table_count == 1U);
    assert(manager.table[0].source == PS_SOURCE_WIFI);
    assert(strcmp(manager.table[0].mac.str, "00:AA:BB:CC:DD:EE") == 0);
    assert(manager.table[0].rssi_last_dbm == -42);
    printf("PASS: test_wifi_parser_to_observation_manager\n");
    return 0;
}

int main(void)
{
    printf("=== test_wifi_scanner ===\n");
    int failed = 0;
    failed += test_valid_probe_request_with_rssi();
    failed += test_valid_probe_request_without_rssi();
    failed += test_valid_management_source_extraction();
    failed += test_valid_variable_information_elements();
    failed += test_truncated_radiotap_header();
    failed += test_invalid_radiotap_lengths();
    failed += test_truncated_80211_header();
    failed += test_invalid_frame_length();
    failed += test_non_management_frame();
    failed += test_management_frame_not_probe_request();
    failed += test_truncated_probe_request();
    failed += test_malformed_information_element();
    failed += test_randomized_address_hint();
    failed += test_broadcast_source_rejected();
    failed += test_invalid_multicast_source_rejected();
    failed += test_rssi_edge_cases();
    failed += test_extended_radiotap_present_bitmap();
    failed += test_unsupported_radiotap_field_is_skipped_safely();
    failed += test_multiple_captured_frames();
    failed += test_empty_capture_buffer();
    failed += test_wifi_parser_to_observation_manager();
    if (failed != 0) return 1;
    printf("All Wi-Fi tests PASSED.\n");
    return 0;
}
