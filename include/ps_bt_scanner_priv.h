#ifndef PS_BT_SCANNER_PRIV_H
#define PS_BT_SCANNER_PRIV_H

#include "ps_observation.h"
#include "ps_types.h"
#include <stddef.h>
#include <stdint.h>

/* Visible for testing ONLY. */
ps_status_t ps_bt_parse_hci_le_adv_report(const uint8_t *buf, size_t len, ps_observation_t *out);

#endif /* PS_BT_SCANNER_PRIV_H */
