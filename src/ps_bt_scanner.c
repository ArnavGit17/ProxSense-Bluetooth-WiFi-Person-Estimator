/**
 * @file ps_bt_scanner.c
 * @brief Bluetooth HCI scanner — Phase 2 implementation.
 *
 * Implements raw Linux HCI sockets to passively scan for LE advertising
 * reports without relying on D-Bus or external daemon APIs.
 */

#include "ps_bt_scanner.h"
#include "ps_bt_scanner_priv.h"
#include "ps_log.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <poll.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

/* Private context */
typedef struct bt_priv {
    ps_bt_scanner_config_t config;
    int                    hci_fd;  /* HCI socket file descriptor; -1 if not open */
    int                    dev_id;  /* Discovered/assigned HCI device ID */
    
    /* Buffer for leftover events if num_reports > 1. In Phase 2, we just 
     * process the first report of each event to simplify, as >99% of events 
     * have num_reports == 1. */
} bt_priv_t;

/* Forward declarations of vtable functions. */
static ps_status_t  bt_open(ps_scanner_t *s);
static ps_status_t  bt_read_next(ps_scanner_t *s, ps_observation_t *out, int timeout_ms);
static void         bt_close(ps_scanner_t *s);
static const char  *bt_describe(const ps_scanner_t *s);

static const ps_scanner_ops_t bt_ops = {
    .open       = bt_open,
    .read_next  = bt_read_next,
    .close      = bt_close,
    .describe   = bt_describe
};

ps_bt_scanner_config_t ps_bt_scanner_default_config(void)
{
    return (ps_bt_scanner_config_t){
        .hci_dev_id         = PS_BT_SCANNER_DEFAULT_HCI,
        .scan_mode          = PS_BT_SCAN_PASSIVE,
        /* Scan interval 100 ms, scan window 50 ms — balanced for energy/coverage. */
        .scan_interval_slots = 160, /* 160 * 0.625 ms = 100 ms */
        .scan_window_slots   =  80  /*  80 * 0.625 ms =  50 ms */
    };
}

ps_status_t ps_bt_scanner_init(ps_scanner_t *scanner,
                               const ps_bt_scanner_config_t *config)
{
    if (!scanner) return PS_ERR_INVALID_ARG;

    bt_priv_t *priv = calloc(1, sizeof(bt_priv_t));
    if (!priv) return PS_ERR_ALLOC;

    priv->config = config ? *config : ps_bt_scanner_default_config();
    priv->hci_fd = -1;
    priv->dev_id = -1;

    scanner->ops     = &bt_ops;
    scanner->priv    = priv;
    scanner->source  = PS_SOURCE_BLUETOOTH;
    scanner->is_open = false;

    PS_LOG_DEBUG("Bluetooth scanner initialised (hci_dev_id=%d, %s scan)",
                 priv->config.hci_dev_id,
                 priv->config.scan_mode == PS_BT_SCAN_PASSIVE ? "passive" : "active");
    return PS_OK;
}

static ps_status_t bt_open(ps_scanner_t *s)
{
    bt_priv_t *priv = s->priv;
    if (s->is_open) return PS_OK;

    int dev_id = priv->config.hci_dev_id;
    if (dev_id == PS_BT_SCANNER_DEFAULT_HCI) {
        /* Auto-discover the first available controller */
        dev_id = hci_get_route(NULL);
        if (dev_id < 0) {
            PS_LOG_ERROR("Bluetooth scanner: No HCI controller found.");
            return PS_ERR_NO_HARDWARE;
        }
    }
    priv->dev_id = dev_id;

    /* Open raw HCI socket */
    int fd = hci_open_dev(dev_id);
    if (fd < 0) {
        if (errno == EPERM || errno == EACCES) {
            PS_LOG_ERROR("Bluetooth scanner: Permission denied on hci%d. "
                         "CAP_NET_RAW or root is required.", dev_id);
            return PS_ERR_PERMISSION;
        }
        PS_LOG_ERROR("Bluetooth scanner: Failed to open hci%d: %s",
                     dev_id, strerror(errno));
        return PS_ERR_NO_HARDWARE;
    }

    priv->hci_fd = fd;

    /* Configure LE scan parameters */
    le_set_scan_parameters_cp scan_params_cp;
    memset(&scan_params_cp, 0, sizeof(scan_params_cp));
    scan_params_cp.type = (priv->config.scan_mode == PS_BT_SCAN_PASSIVE) ? 0x00 : 0x01;
    scan_params_cp.interval = htobs(priv->config.scan_interval_slots);
    scan_params_cp.window = htobs(priv->config.scan_window_slots);
    scan_params_cp.own_bdaddr_type = 0x00; /* Public */
    scan_params_cp.filter = 0x00; /* Accept all */

    struct hci_request rq;
    memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_SCAN_PARAMETERS;
    rq.cparam = &scan_params_cp;
    rq.clen = LE_SET_SCAN_PARAMETERS_CP_SIZE;
    rq.rparam = NULL;
    rq.rlen = 0;

    if (hci_send_req(fd, &rq, 1000) < 0) {
        PS_LOG_ERROR("Bluetooth scanner: Failed to set LE scan parameters: %s", strerror(errno));
        close(fd);
        priv->hci_fd = -1;
        return PS_ERR_IO;
    }

    /* Enable LE scanning */
    le_set_scan_enable_cp scan_enable_cp;
    memset(&scan_enable_cp, 0, sizeof(scan_enable_cp));
    scan_enable_cp.enable = 0x01;
    scan_enable_cp.filter_dup = 0x00; /* Disable duplicate filtering to receive continuous RSSI */

    memset(&rq, 0, sizeof(rq));
    rq.ogf = OGF_LE_CTL;
    rq.ocf = OCF_LE_SET_SCAN_ENABLE;
    rq.cparam = &scan_enable_cp;
    rq.clen = LE_SET_SCAN_ENABLE_CP_SIZE;

    if (hci_send_req(fd, &rq, 1000) < 0) {
        PS_LOG_ERROR("Bluetooth scanner: Failed to enable LE scan: %s", strerror(errno));
        close(fd);
        priv->hci_fd = -1;
        return PS_ERR_IO;
    }

    /* Filter the socket to ONLY receive LE Meta Events to avoid wakeups for unrelated BT activity. */
    struct hci_filter flt;
    hci_filter_clear(&flt);
    hci_filter_set_ptype(HCI_EVENT_PKT, &flt);
    hci_filter_set_event(EVT_LE_META_EVENT, &flt);
    if (setsockopt(fd, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
        PS_LOG_ERROR("Bluetooth scanner: Failed to set socket filter: %s", strerror(errno));
        /* Best effort disable scan */
        scan_enable_cp.enable = 0x00;
        hci_send_req(fd, &rq, 1000);
        close(fd);
        priv->hci_fd = -1;
        return PS_ERR_IO;
    }

    s->is_open = true;
    PS_LOG_INFO("Bluetooth scanner started on hci%d", priv->dev_id);
    return PS_OK;
}

ps_status_t ps_bt_parse_hci_le_adv_report(const uint8_t *buf, size_t len, ps_observation_t *out)
{
    if (!buf || !out) return PS_ERR_INVALID_ARG;

    ps_observation_init(out);

    if (len < 5U) {
        PS_LOG_WARN("Bluetooth scanner: Truncated LE Meta Event");
        return PS_ERR_TIMEOUT;
    }

    if (buf[0] != HCI_EVENT_PKT) {
        PS_LOG_WARN("Bluetooth scanner: Packet is not an HCI event");
        return PS_ERR_TIMEOUT;
    }

    if (buf[1] != EVT_LE_META_EVENT) {
        PS_LOG_WARN("Bluetooth scanner: Unsupported HCI event %02x", buf[1]);
        return PS_ERR_TIMEOUT;
    }

    size_t event_len = (size_t)buf[2];
    size_t event_end = 3U + event_len;
    if (len < event_end) {
        PS_LOG_WARN("Bluetooth scanner: Truncated LE Meta Event");
        return PS_ERR_TIMEOUT;
    }

    size_t p = 3U;
    if (buf[p] != EVT_LE_ADVERTISING_REPORT) {
        PS_LOG_WARN("Bluetooth scanner: Unsupported LE subevent %02x", buf[p]);
        return PS_ERR_TIMEOUT;
    }
    p += 1U;

    if (p >= event_end) {
        PS_LOG_WARN("Bluetooth scanner: Missing LE report count");
        return PS_ERR_TIMEOUT;
    }

    uint8_t num_reports = buf[p++];
    if (num_reports == 0U) {
        PS_LOG_WARN("Bluetooth scanner: Empty LE advertising report set");
        return PS_ERR_TIMEOUT;
    }

    for (uint8_t report_index = 0; report_index < num_reports; ++report_index) {
        if (p + 1U + 1U + 6U + 1U > event_end) {
            PS_LOG_WARN("Bluetooth scanner: Truncated advertising report");
            return PS_ERR_TIMEOUT;
        }

        uint8_t event_type = buf[p++];
        uint8_t addr_type = buf[p++];
        const uint8_t *bdaddr = &buf[p];
        p += 6U;
        uint8_t adv_len = buf[p++];

        if (p + adv_len + 1U > event_end) {
            PS_LOG_WARN("Bluetooth scanner: Advertising report length exceeds event buffer");
            return PS_ERR_TIMEOUT;
        }

        const uint8_t *adv = &buf[p];
        p += adv_len;
        int8_t rssi = (int8_t)buf[p++];

        /* Ignore other reports after the first valid one. */
        out->source = PS_SOURCE_BLUETOOTH;
        out->meta.bt.is_classic = false;
        out->meta.bt.adv_type = event_type;

        /* Valid BLE RSSI is in [-127, +20]. 0 dBm is a real reading and must be
         * preserved. The common "RSSI not available" sentinel on Linux is 0x7F.
         */
        out->rssi_dbm = (rssi == 127) ? PS_RSSI_UNAVAILABLE : (int8_t)rssi;

        snprintf(out->mac.str, sizeof(out->mac.str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 bdaddr[5], bdaddr[4], bdaddr[3], bdaddr[2], bdaddr[1], bdaddr[0]);

        switch (addr_type & 0x03U) {
            case 0x00:
            case 0x02:
                out->meta.bt.addr_type = PS_BT_ADDR_PUBLIC;
                break;
            case 0x01:
            case 0x03:
                out->meta.bt.addr_type = PS_BT_ADDR_RANDOM;
                break;
            default:
                out->meta.bt.addr_type = PS_BT_ADDR_UNKNOWN;
                break;
        }

        size_t ad_offset = 0U;
        while (ad_offset < (size_t)adv_len) {
            if (ad_offset + 1U > (size_t)adv_len) break;
            uint8_t field_len = adv[ad_offset];
            if (field_len == 0U) break;
            if (ad_offset + 1U + field_len > (size_t)adv_len) {
                PS_LOG_DEBUG("Bluetooth scanner: AD structure length exceeds buffer");
                break;
            }

            uint8_t ad_type = adv[ad_offset + 1U];
            if (ad_type == 0x08U || ad_type == 0x09U) {
                size_t name_len = (size_t)field_len - 1U;
                if (name_len >= sizeof(out->name)) {
                    name_len = sizeof(out->name) - 1U;
                }
                memcpy(out->name, adv + ad_offset + 2U, name_len);
                out->name[name_len] = '\0';
                break;
            }

            ad_offset += (size_t)field_len + 1U;
        }

        return PS_OK;
    }

    PS_LOG_WARN("Bluetooth scanner: No valid LE advertising reports found");
    return PS_ERR_TIMEOUT;
}

static ps_status_t bt_read_next(ps_scanner_t *s, ps_observation_t *out, int timeout_ms)
{
    bt_priv_t *priv = s->priv;
    if (!s->is_open || priv->hci_fd < 0) return PS_ERR_IO;

    struct pollfd pfd;
    pfd.fd = priv->hci_fd;
    pfd.events = POLLIN;

    int poll_res = poll(&pfd, 1, timeout_ms);
    if (poll_res < 0) {
        if (errno == EINTR) return PS_ERR_TIMEOUT;
        return PS_ERR_IO;
    }
    if (poll_res == 0) return PS_ERR_TIMEOUT;

    uint8_t buf[HCI_MAX_EVENT_SIZE];
    ssize_t len = read(priv->hci_fd, buf, sizeof(buf));
    if (len < 0) {
        if (errno == EAGAIN || errno == EINTR) return PS_ERR_TIMEOUT;
        return PS_ERR_IO;
    }

    /* Pass to the parser */
    return ps_bt_parse_hci_le_adv_report(buf, (size_t)len, out);
}

static void bt_close(ps_scanner_t *s)
{
    if (!s) return;
    bt_priv_t *priv = s->priv;
    if (priv && s->is_open && priv->hci_fd >= 0) {
        /* Best effort disable LE scanning to leave controller in good state */
        struct hci_request rq;
        le_set_scan_enable_cp scan_enable_cp;
        memset(&scan_enable_cp, 0, sizeof(scan_enable_cp));
        scan_enable_cp.enable = 0x00; /* Disable */
        scan_enable_cp.filter_dup = 0x00;

        memset(&rq, 0, sizeof(rq));
        rq.ogf = OGF_LE_CTL;
        rq.ocf = OCF_LE_SET_SCAN_ENABLE;
        rq.cparam = &scan_enable_cp;
        rq.clen = LE_SET_SCAN_ENABLE_CP_SIZE;
        rq.rparam = NULL;
        rq.rlen = 0;

        /* Ignore errors here as we are closing anyway */
        hci_send_req(priv->hci_fd, &rq, 1000);

        close(priv->hci_fd);
        priv->hci_fd = -1;
        PS_LOG_INFO("Bluetooth scanner closed (hci%d)", priv->dev_id);
    }
    if (priv) {
        free(priv);
        s->priv = NULL;
    }
    s->is_open = false;
}

static const char *bt_describe(const ps_scanner_t *s)
{
    bt_priv_t *priv = (s && s->priv) ? s->priv : NULL;
    static char desc[128];
    if (priv && priv->dev_id >= 0) {
        snprintf(desc, sizeof(desc), "Bluetooth HCI scanner (hci%d, passive LE scan)", priv->dev_id);
        return desc;
    }
    return "Bluetooth HCI scanner (raw AF_BLUETOOTH, LE scan)";
}
