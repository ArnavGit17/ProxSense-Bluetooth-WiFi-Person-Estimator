#ifndef PS_WIFI_SCANNER_PRIV_H
#define PS_WIFI_SCANNER_PRIV_H

#include "ps_observation.h"
#include "ps_types.h"
#include <stddef.h>
#include <stdint.h>

/* Visible for deterministic packet-parser tests. */
ps_status_t ps_wifi_parse_radiotap_probe_request(const uint8_t *packet,
                                                size_t packet_len,
                                                ps_observation_t *out);

#endif /* PS_WIFI_SCANNER_PRIV_H */