#define _DEFAULT_SOURCE

#include "ps_wifi_scanner.h"
#include "ps_wifi_scanner_priv.h"
#include "ps_log.h"

#include <dirent.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WIFI_NL_BUFFER_SIZE 4096U
#define WIFI_CAPTURE_BUFFER_SIZE 4096U
#define WIFI_MAX_PRESENT_WORDS 8U
#define WIFI_NL80211_FAMILY "nl80211"

#define WIFI_NLA_ALIGNTO 4U
#define WIFI_NLA_ALIGN(length) (((length) + WIFI_NLA_ALIGNTO - 1U) & ~(WIFI_NLA_ALIGNTO - 1U))
#define WIFI_NLA_HDRLEN WIFI_NLA_ALIGN(sizeof(struct nlattr))

struct wifi_priv {
    ps_wifi_scanner_config_t config;
    int netlink_fd;
    int packet_fd;
    int base_ifindex;
    int capture_ifindex;
    int nl80211_family;
    uint32_t netlink_port;
    uint32_t sequence;
    bool owns_monitor_interface;
    bool brought_iface_up;
    char base_iface[PS_IFACE_NAME_LEN];
    char monitor_iface[PS_IFACE_NAME_LEN];
};

static ps_status_t wifi_open(ps_scanner_t *scanner);
static ps_status_t wifi_read_next(ps_scanner_t *scanner,
                                  ps_observation_t *out,
                                  int timeout_ms);
static void wifi_close(ps_scanner_t *scanner);
static const char *wifi_describe(const ps_scanner_t *scanner);

static const ps_scanner_ops_t wifi_ops = {
    .open = wifi_open,
    .read_next = wifi_read_next,
    .close = wifi_close,
    .describe = wifi_describe
};

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint8_t channel_from_frequency(uint16_t frequency_mhz)
{
    if (frequency_mhz == 2484U) return 14U;
    if (frequency_mhz >= 2412U && frequency_mhz <= 2472U &&
        (frequency_mhz - 2407U) % 5U == 0U) {
        return (uint8_t)((frequency_mhz - 2407U) / 5U);
    }
    if (frequency_mhz >= 5000U && frequency_mhz < 5950U &&
        frequency_mhz % 5U == 0U) {
        return (uint8_t)((frequency_mhz - 5000U) / 5U);
    }
    if (frequency_mhz >= 5955U && (frequency_mhz - 5950U) % 5U == 0U) {
        return (uint8_t)((frequency_mhz - 5950U) / 5U);
    }
    return 0U;
}

static ps_status_t parse_radiotap(const uint8_t *packet,
                                  size_t packet_len,
                                  size_t *frame_offset,
                                  int8_t *rssi,
                                  uint8_t *channel)
{
    if (packet_len < 8U || packet[0] != 0U) return PS_ERR_MALFORMED;

    size_t radiotap_len = (size_t)read_le16(packet + 2U);
    if (radiotap_len < 8U || radiotap_len > packet_len) return PS_ERR_MALFORMED;

    uint32_t present[WIFI_MAX_PRESENT_WORDS];
    size_t word_count = 0U;
    size_t bitmap_offset = 4U;
    bool has_extension;
    do {
        if (word_count >= WIFI_MAX_PRESENT_WORDS ||
            bitmap_offset + 4U > radiotap_len) {
            return PS_ERR_MALFORMED;
        }
        present[word_count] = read_le32(packet + bitmap_offset);
        has_extension = (present[word_count] & 0x80000000U) != 0U;
        word_count++;
        bitmap_offset += 4U;
    } while (has_extension);

    size_t field_offset = bitmap_offset;
    *rssi = PS_RSSI_UNAVAILABLE;
    *channel = 0U;

    /* Fields 0-5 have fixed, specified radiotap alignment and size. */
    for (size_t field = 0U; field <= 5U; ++field) {
        size_t word = field / 32U;
        size_t bit = field % 32U;
        if (word >= word_count || (present[word] & (1U << bit)) == 0U) continue;

        size_t alignment;
        size_t field_size;
        switch (field) {
            case 0U: alignment = 8U; field_size = 8U; break; /* TSFT */
            case 1U: alignment = 1U; field_size = 1U; break; /* Flags */
            case 2U: alignment = 1U; field_size = 1U; break; /* Rate */
            case 3U: alignment = 2U; field_size = 4U; break; /* Channel */
            case 4U: alignment = 2U; field_size = 2U; break; /* FHSS */
            default: alignment = 1U; field_size = 1U; break; /* dBm signal */
        }
        field_offset = (field_offset + alignment - 1U) & ~(alignment - 1U);
        if (field_offset > radiotap_len || field_size > radiotap_len - field_offset) {
            return PS_ERR_MALFORMED;
        }
        if (field == 3U) *channel = channel_from_frequency(read_le16(packet + field_offset));
        if (field == 5U) {
            int8_t signal = (int8_t)packet[field_offset];
            *rssi = (signal >= -127 && signal <= 20) ?
                signal : PS_RSSI_UNAVAILABLE;
        }
        field_offset += field_size;
    }

    *frame_offset = radiotap_len;
    return PS_OK;
}

ps_status_t ps_wifi_parse_radiotap_probe_request(const uint8_t *packet,
                                                size_t packet_len,
                                                ps_observation_t *out)
{
    if (!packet || !out) return PS_ERR_INVALID_ARG;
    ps_observation_init(out);

    size_t frame_offset = 0U;
    int8_t rssi = PS_RSSI_UNAVAILABLE;
    uint8_t channel = 0U;
    ps_status_t status = parse_radiotap(packet, packet_len, &frame_offset,
                                        &rssi, &channel);
    if (status != PS_OK) return status;

    if (frame_offset > packet_len || packet_len - frame_offset < 24U) {
        return PS_ERR_MALFORMED;
    }

    const uint8_t *frame = packet + frame_offset;
    uint16_t frame_control = read_le16(frame);
    uint8_t frame_type = (uint8_t)((frame_control >> 2) & 0x03U);
    uint8_t frame_subtype = (uint8_t)((frame_control >> 4) & 0x0fU);
    if (frame_type != 0U || frame_subtype != 4U) return PS_ERR_UNSUPPORTED;
    if ((frame_control & 0x0003U) != 0U) return PS_ERR_MALFORMED;

    const uint8_t *source = frame + 10U;
    bool all_zero = true;
    bool all_ff = true;
    for (size_t i = 0U; i < 6U; ++i) {
        if (source[i] != 0U) all_zero = false;
        if (source[i] != 0xffU) all_ff = false;
    }
    if (all_zero || all_ff || (source[0] & 0x01U) != 0U) {
        return PS_ERR_MALFORMED;
    }

    size_t ie_offset = frame_offset + 24U;
    bool found_ssid = false;
    while (ie_offset < packet_len) {
        if (packet_len - ie_offset < 2U) return PS_ERR_MALFORMED;
        uint8_t element_id = packet[ie_offset];
        size_t element_len = (size_t)packet[ie_offset + 1U];
        ie_offset += 2U;
        if (element_len > packet_len - ie_offset) return PS_ERR_MALFORMED;
        if (element_id == 0U) {
            if (element_len > 32U) return PS_ERR_MALFORMED;
            found_ssid = true;
        }
        ie_offset += element_len;
    }
    if (!found_ssid) return PS_ERR_MALFORMED;

    out->source = PS_SOURCE_WIFI;
    out->rssi_dbm = rssi;
    out->meta.wifi.frame_type = PS_WIFI_FRAME_PROBE_REQ;
    out->meta.wifi.channel = channel;
    out->meta.wifi.locally_administered = (source[0] & 0x02U) != 0U;
    (void)snprintf(out->mac.str, sizeof(out->mac.str),
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   source[0], source[1], source[2],
                   source[3], source[4], source[5]);
    (void)clock_gettime(CLOCK_MONOTONIC, &out->first_seen);
    out->last_seen = out->first_seen;
    return PS_OK;
}

static bool attr_put(uint8_t *buffer,
                     size_t capacity,
                     size_t *used,
                     uint16_t type,
                     const void *data,
                     size_t data_len)
{
    size_t total = WIFI_NLA_HDRLEN + data_len;
    size_t aligned = WIFI_NLA_ALIGN(total);
    if (*used > capacity || aligned > capacity - *used || total > UINT16_MAX) {
        return false;
    }
    struct nlattr attr = {
        .nla_len = (uint16_t)total,
        .nla_type = type
    };
    memcpy(buffer + *used, &attr, sizeof(attr));
    if (data_len > 0U) memcpy(buffer + *used + WIFI_NLA_HDRLEN, data, data_len);
    if (aligned > total) memset(buffer + *used + total, 0, aligned - total);
    *used += aligned;
    return true;
}

static const struct nlattr *attr_find(const uint8_t *attributes,
                                      size_t attributes_len,
                                      uint16_t wanted_type)
{
    size_t offset = 0U;
    while (offset + sizeof(struct nlattr) <= attributes_len) {
        const struct nlattr *attr = (const struct nlattr *)(const void *)(attributes + offset);
        if (attr->nla_len < sizeof(*attr) || attr->nla_len > attributes_len - offset) {
            return NULL;
        }
        if ((attr->nla_type & NLA_TYPE_MASK) == wanted_type) return attr;
        size_t step = WIFI_NLA_ALIGN((size_t)attr->nla_len);
        if (step > attributes_len - offset) return NULL;
        offset += step;
    }
    return NULL;
}

static ps_status_t nl_request(struct wifi_priv *priv,
                              uint16_t family,
                              uint8_t command,
                              uint16_t flags,
                              const uint8_t *attributes,
                              size_t attributes_len,
                              bool expect_reply,
                              uint8_t *reply,
                              size_t reply_capacity,
                              size_t *reply_len,
                              int *kernel_error)
{
    uint8_t request[WIFI_NL_BUFFER_SIZE];
    size_t header_len = NLMSG_HDRLEN + GENL_HDRLEN;
    if (attributes_len > sizeof(request) - header_len) return PS_ERR_INVALID_ARG;
    memset(request, 0, header_len);

    struct nlmsghdr *nlh = (struct nlmsghdr *)(void *)request;
    nlh->nlmsg_len = (uint32_t)(header_len + attributes_len);
    nlh->nlmsg_type = family;
    nlh->nlmsg_flags = (uint16_t)(NLM_F_REQUEST | flags |
                                  (expect_reply ? 0U : NLM_F_ACK));
    nlh->nlmsg_seq = ++priv->sequence;
    nlh->nlmsg_pid = priv->netlink_port;

    struct genlmsghdr *genl = (struct genlmsghdr *)(void *)(request + NLMSG_HDRLEN);
    genl->cmd = command;
    genl->version = 1U;
    if (attributes_len > 0U) memcpy(request + header_len, attributes, attributes_len);

    struct sockaddr_nl kernel_address;
    memset(&kernel_address, 0, sizeof(kernel_address));
    kernel_address.nl_family = AF_NETLINK;
    struct iovec iov = { .iov_base = request, .iov_len = nlh->nlmsg_len };
    struct msghdr message;
    memset(&message, 0, sizeof(message));
    message.msg_name = &kernel_address;
    message.msg_namelen = sizeof(kernel_address);
    message.msg_iov = &iov;
    message.msg_iovlen = 1U;
    if (sendmsg(priv->netlink_fd, &message, 0) < 0) {
        if (kernel_error) *kernel_error = errno;
        return (errno == EPERM || errno == EACCES) ? PS_ERR_PERMISSION : PS_ERR_IO;
    }

    uint8_t response[WIFI_NL_BUFFER_SIZE];
    for (;;) {
        ssize_t received = recv(priv->netlink_fd, response, sizeof(response), 0);
        if (received < 0) {
            if (errno == EINTR) continue;
            if (kernel_error) *kernel_error = errno;
            return (errno == EPERM || errno == EACCES) ? PS_ERR_PERMISSION : PS_ERR_IO;
        }
        if ((size_t)received < sizeof(struct nlmsghdr)) return PS_ERR_MALFORMED;

        size_t remaining = (size_t)received;
        for (struct nlmsghdr *answer = (struct nlmsghdr *)(void *)response;
             NLMSG_OK(answer, remaining);
             answer = NLMSG_NEXT(answer, remaining)) {
            if (answer->nlmsg_seq != nlh->nlmsg_seq) continue;
            if (answer->nlmsg_type == NLMSG_ERROR) {
                if (answer->nlmsg_len < NLMSG_LENGTH(sizeof(struct nlmsgerr))) {
                    return PS_ERR_MALFORMED;
                }
                const struct nlmsgerr *error = (const struct nlmsgerr *)NLMSG_DATA(answer);
                if (error->error == 0) {
                    if (expect_reply) continue;
                    return PS_OK;
                }
                int code = -error->error;
                if (kernel_error) *kernel_error = code;
                if (code == EPERM || code == EACCES) return PS_ERR_PERMISSION;
                if (code == ENODEV || code == ENXIO) return PS_ERR_NO_HARDWARE;
                if (code == EOPNOTSUPP || code == ENOTSUP || code == EINVAL ||
                    code == EBUSY || code == ENOSPC) return PS_ERR_UNSUPPORTED;
                return PS_ERR_IO;
            }
            if (expect_reply) {
                size_t payload_len = (size_t)answer->nlmsg_len - NLMSG_HDRLEN;
                if (payload_len > reply_capacity) return PS_ERR_MALFORMED;
                memcpy(reply, NLMSG_DATA(answer), payload_len);
                *reply_len = payload_len;
                return PS_OK;
            }
        }
    }
}

static ps_status_t resolve_nl80211(struct wifi_priv *priv)
{
    uint8_t attrs[64];
    size_t attrs_len = 0U;
    size_t family_name_len = sizeof(WIFI_NL80211_FAMILY);
    if (!attr_put(attrs, sizeof(attrs), &attrs_len, CTRL_ATTR_FAMILY_NAME,
                  WIFI_NL80211_FAMILY, family_name_len)) return PS_ERR_IO;

    uint8_t reply[256];
    size_t reply_len = 0U;
    int kernel_error = 0;
    ps_status_t status = nl_request(priv, GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
                                    0U, attrs, attrs_len, true, reply,
                                    sizeof(reply), &reply_len, &kernel_error);
    if (status != PS_OK) return status == PS_ERR_IO ? PS_ERR_UNSUPPORTED : status;
    if (reply_len < GENL_HDRLEN) return PS_ERR_MALFORMED;
    const struct nlattr *family = attr_find(reply + GENL_HDRLEN,
                                             reply_len - GENL_HDRLEN,
                                             CTRL_ATTR_FAMILY_ID);
    if (!family || family->nla_len < WIFI_NLA_HDRLEN + sizeof(uint16_t)) {
        return PS_ERR_MALFORMED;
    }
    memcpy(&priv->nl80211_family,
           (const uint8_t *)family + WIFI_NLA_HDRLEN, sizeof(uint16_t));
    return PS_OK;
}

static ps_status_t open_netlink(struct wifi_priv *priv)
{
    priv->netlink_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (priv->netlink_fd < 0) {
        return (errno == EPERM || errno == EACCES) ? PS_ERR_PERMISSION : PS_ERR_IO;
    }
    struct sockaddr_nl local;
    memset(&local, 0, sizeof(local));
    local.nl_family = AF_NETLINK;
    if (bind(priv->netlink_fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ps_status_t status = (errno == EPERM || errno == EACCES) ? PS_ERR_PERMISSION : PS_ERR_IO;
        close(priv->netlink_fd);
        priv->netlink_fd = -1;
        return status;
    }
    struct timeval receive_timeout = { .tv_sec = 3, .tv_usec = 0 };
    if (setsockopt(priv->netlink_fd, SOL_SOCKET, SO_RCVTIMEO,
                   &receive_timeout, sizeof(receive_timeout)) < 0) {
        close(priv->netlink_fd);
        priv->netlink_fd = -1;
        return PS_ERR_IO;
    }
    socklen_t local_len = sizeof(local);
    if (getsockname(priv->netlink_fd, (struct sockaddr *)&local, &local_len) < 0) {
        close(priv->netlink_fd);
        priv->netlink_fd = -1;
        return PS_ERR_IO;
    }
    priv->netlink_port = local.nl_pid;
    return resolve_nl80211(priv);
}

static int find_wireless_interface(char *name, size_t capacity)
{
    DIR *directory = opendir("/sys/class/net");
    if (!directory) return -1;
    struct dirent *entry;
    int result = -1;
    while ((entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[256];
        int written = snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) continue;
        if (access(path, F_OK) == 0) {
            size_t name_len = strlen(entry->d_name);
            if (name_len < capacity) {
                memcpy(name, entry->d_name, name_len + 1U);
                result = (int)if_nametoindex(name);
                break;
            }
        }
    }
    closedir(directory);
    return result;
}

static ps_status_t get_interface_info(struct wifi_priv *priv,
                                      int ifindex,
                                      uint32_t *wiphy,
                                      uint32_t *iftype)
{
    uint8_t attrs[32];
    size_t attrs_len = 0U;
    uint32_t index = (uint32_t)ifindex;
    if (!attr_put(attrs, sizeof(attrs), &attrs_len, NL80211_ATTR_IFINDEX,
                  &index, sizeof(index))) return PS_ERR_IO;
    uint8_t reply[512];
    size_t reply_len = 0U;
    ps_status_t status = nl_request(priv, (uint16_t)priv->nl80211_family,
                                    NL80211_CMD_GET_INTERFACE, 0U,
                                    attrs, attrs_len, true, reply, sizeof(reply),
                                    &reply_len, NULL);
    if (status != PS_OK) return status;
    if (reply_len < GENL_HDRLEN) return PS_ERR_MALFORMED;
    const uint8_t *reply_attrs = reply + GENL_HDRLEN;
    size_t attrs_size = reply_len - GENL_HDRLEN;
    const struct nlattr *wiphy_attr = attr_find(reply_attrs, attrs_size, NL80211_ATTR_WIPHY);
    const struct nlattr *iftype_attr = attr_find(reply_attrs, attrs_size, NL80211_ATTR_IFTYPE);
    if (!wiphy_attr || !iftype_attr ||
        wiphy_attr->nla_len < WIFI_NLA_HDRLEN + sizeof(*wiphy) ||
        iftype_attr->nla_len < WIFI_NLA_HDRLEN + sizeof(*iftype)) return PS_ERR_MALFORMED;
    memcpy(wiphy, (const uint8_t *)wiphy_attr + WIFI_NLA_HDRLEN, sizeof(*wiphy));
    memcpy(iftype, (const uint8_t *)iftype_attr + WIFI_NLA_HDRLEN, sizeof(*iftype));
    return PS_OK;
}

static ps_status_t create_monitor_interface(struct wifi_priv *priv, uint32_t wiphy)
{
    uint8_t attrs[128];
    size_t attrs_len = 0U;
    uint32_t iftype = NL80211_IFTYPE_MONITOR;
    char candidate[PS_IFACE_NAME_LEN];
    int written = snprintf(candidate, sizeof(candidate), "psmon%u",
                           (unsigned)(getpid() % 10000));
    if (written < 0 || (size_t)written >= sizeof(candidate)) return PS_ERR_IO;
    if (!attr_put(attrs, sizeof(attrs), &attrs_len, NL80211_ATTR_WIPHY,
                  &wiphy, sizeof(wiphy)) ||
        !attr_put(attrs, sizeof(attrs), &attrs_len, NL80211_ATTR_IFNAME,
                  candidate, strlen(candidate) + 1U) ||
        !attr_put(attrs, sizeof(attrs), &attrs_len, NL80211_ATTR_IFTYPE,
                  &iftype, sizeof(iftype))) return PS_ERR_IO;

    ps_status_t status = nl_request(priv, (uint16_t)priv->nl80211_family,
                                    NL80211_CMD_NEW_INTERFACE, NLM_F_CREATE,
                                    attrs, attrs_len, false, NULL, 0U, NULL, NULL);
    if (status != PS_OK) return status;
    (void)strncpy(priv->monitor_iface, candidate, sizeof(priv->monitor_iface) - 1U);
    priv->owns_monitor_interface = true;
    priv->capture_ifindex = (int)if_nametoindex(priv->monitor_iface);
    if (priv->capture_ifindex == 0) return PS_ERR_IO;
    return PS_OK;
}

static ps_status_t set_interface_up(const char *name, bool *changed_state)
{
    int control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (control_fd < 0) return PS_ERR_IO;
    struct ifreq request;
    memset(&request, 0, sizeof(request));
    (void)strncpy(request.ifr_name, name, sizeof(request.ifr_name) - 1U);
    if (ioctl(control_fd, SIOCGIFFLAGS, &request) < 0) {
        close(control_fd);
        return (errno == EPERM || errno == EACCES) ? PS_ERR_PERMISSION : PS_ERR_IO;
    }
    if ((request.ifr_flags & IFF_UP) == 0) {
        request.ifr_flags = (short)(request.ifr_flags | IFF_UP);
        if (ioctl(control_fd, SIOCSIFFLAGS, &request) < 0) {
            ps_status_t status = (errno == EPERM || errno == EACCES) ?
                                 PS_ERR_PERMISSION : PS_ERR_IO;
            close(control_fd);
            return status;
        }
        if (changed_state) *changed_state = true;
    }
    close(control_fd);
    return PS_OK;
}

static void delete_monitor_interface(struct wifi_priv *priv)
{
    if (!priv->owns_monitor_interface || priv->netlink_fd < 0) return;
    if (priv->capture_ifindex <= 0 && priv->monitor_iface[0] != '\0') {
        priv->capture_ifindex = (int)if_nametoindex(priv->monitor_iface);
    }
    if (priv->capture_ifindex <= 0) return;
    uint32_t ifindex = (uint32_t)priv->capture_ifindex;
    uint8_t attrs[32];
    size_t attrs_len = 0U;
    if (attr_put(attrs, sizeof(attrs), &attrs_len, NL80211_ATTR_IFINDEX,
                 &ifindex, sizeof(ifindex))) {
        (void)nl_request(priv, (uint16_t)priv->nl80211_family,
                         NL80211_CMD_DEL_INTERFACE, 0U,
                         attrs, attrs_len, false, NULL, 0U, NULL, NULL);
    }
    priv->owns_monitor_interface = false;
    priv->capture_ifindex = 0;
}

ps_wifi_scanner_config_t ps_wifi_scanner_default_config(void)
{
    ps_wifi_scanner_config_t config;
    memset(&config, 0, sizeof(config));
    config.op_mode = PS_WIFI_MODE_MONITOR;
    config.scan_interval_s = 10;
    return config;
}

ps_status_t ps_wifi_scanner_init(ps_scanner_t *scanner,
                                 const ps_wifi_scanner_config_t *config)
{
    if (!scanner) return PS_ERR_INVALID_ARG;
    struct wifi_priv *priv = calloc(1U, sizeof(*priv));
    if (!priv) return PS_ERR_ALLOC;
    priv->config = config ? *config : ps_wifi_scanner_default_config();
    if (memchr(priv->config.iface, '\0', sizeof(priv->config.iface)) == NULL) {
        free(priv);
        return PS_ERR_INVALID_ARG;
    }
    priv->netlink_fd = -1;
    priv->packet_fd = -1;
    scanner->ops = &wifi_ops;
    scanner->priv = priv;
    scanner->source = PS_SOURCE_WIFI;
    scanner->is_open = false;
    return PS_OK;
}

static ps_status_t wifi_open(ps_scanner_t *scanner)
{
    struct wifi_priv *priv = scanner->priv;
    if (scanner->is_open) return PS_OK;

    if (priv->config.iface[0] != '\0') {
        (void)strncpy(priv->base_iface, priv->config.iface, sizeof(priv->base_iface) - 1U);
        priv->base_ifindex = (int)if_nametoindex(priv->base_iface);
        if (priv->base_ifindex == 0) {
            PS_LOG_ERROR("Wi-Fi interface '%s' does not exist", priv->base_iface);
            return PS_ERR_NO_HARDWARE;
        }
    } else {
        priv->base_ifindex = find_wireless_interface(priv->base_iface,
                                                      sizeof(priv->base_iface));
        if (priv->base_ifindex <= 0) {
            PS_LOG_ERROR("No Linux wireless interface with nl80211 support was found");
            return PS_ERR_NO_HARDWARE;
        }
    }

    ps_status_t status = open_netlink(priv);
    if (status != PS_OK) {
        PS_LOG_ERROR("Wi-Fi nl80211 unavailable: %s", ps_status_str(status));
        wifi_close(scanner);
        return status;
    }

    uint32_t wiphy = 0U;
    uint32_t iftype = 0U;
    status = get_interface_info(priv, priv->base_ifindex, &wiphy, &iftype);
    if (status != PS_OK) {
        PS_LOG_ERROR("Interface '%s' is not available through nl80211: %s",
                     priv->base_iface, ps_status_str(status));
        wifi_close(scanner);
        return status == PS_ERR_NO_HARDWARE ? PS_ERR_UNSUPPORTED : status;
    }

    if (iftype == NL80211_IFTYPE_MONITOR) {
        priv->capture_ifindex = priv->base_ifindex;
        (void)strncpy(priv->monitor_iface, priv->base_iface,
                      sizeof(priv->monitor_iface) - 1U);
    } else {
        status = create_monitor_interface(priv, wiphy);
        if (status != PS_OK) {
            if (status == PS_ERR_UNSUPPORTED) {
                PS_LOG_ERROR("Wi-Fi driver does not permit monitor-mode interface creation on '%s'",
                             priv->base_iface);
            } else if (status == PS_ERR_PERMISSION) {
                PS_LOG_ERROR("Permission denied enabling Wi-Fi monitor mode; CAP_NET_ADMIN is required");
            } else {
                PS_LOG_ERROR("Failed to create Wi-Fi monitor interface: %s",
                             ps_status_str(status));
            }
            wifi_close(scanner);
            return status;
        }
    }

    status = set_interface_up(priv->monitor_iface, &priv->brought_iface_up);
    if (status != PS_OK) {
        PS_LOG_ERROR("Cannot bring monitor interface '%s' up: %s",
                     priv->monitor_iface, ps_status_str(status));
        wifi_close(scanner);
        return status;
    }

    priv->packet_fd = socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC,
                             htons(ETH_P_ALL));
    if (priv->packet_fd < 0) {
        status = (errno == EPERM || errno == EACCES) ?
                 PS_ERR_PERMISSION : PS_ERR_IO;
        PS_LOG_ERROR("Cannot open Wi-Fi packet capture socket: %s",
                     status == PS_ERR_PERMISSION ? "CAP_NET_RAW is required" : strerror(errno));
        wifi_close(scanner);
        return status;
    }
    struct sockaddr_ll capture_address;
    memset(&capture_address, 0, sizeof(capture_address));
    capture_address.sll_family = AF_PACKET;
    capture_address.sll_protocol = htons(ETH_P_ALL);
    capture_address.sll_ifindex = priv->capture_ifindex;
    if (bind(priv->packet_fd, (struct sockaddr *)&capture_address,
             sizeof(capture_address)) < 0) {
        status = (errno == EPERM || errno == EACCES) ?
                 PS_ERR_PERMISSION : PS_ERR_IO;
        PS_LOG_ERROR("Cannot bind Wi-Fi capture socket to '%s': %s",
                     priv->monitor_iface, strerror(errno));
        wifi_close(scanner);
        return status;
    }

    scanner->is_open = true;
    PS_LOG_INFO("Wi-Fi monitor capture started on '%s' (base interface '%s')",
                priv->monitor_iface, priv->base_iface);
    return PS_OK;
}

static ps_status_t wifi_read_next(ps_scanner_t *scanner,
                                  ps_observation_t *out,
                                  int timeout_ms)
{
    if (!scanner || !out || timeout_ms < 0) return PS_ERR_INVALID_ARG;
    struct wifi_priv *priv = scanner->priv;
    if (!scanner->is_open || priv->packet_fd < 0) return PS_ERR_IO;

    struct pollfd descriptor = { .fd = priv->packet_fd, .events = POLLIN };
    int poll_status = poll(&descriptor, 1U, timeout_ms);
    if (poll_status == 0) return PS_ERR_TIMEOUT;
    if (poll_status < 0) return errno == EINTR ? PS_ERR_TIMEOUT : PS_ERR_SCANNER_FAILED;
    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return PS_ERR_SCANNER_FAILED;
    }

    uint8_t packet[WIFI_CAPTURE_BUFFER_SIZE];
    struct sockaddr_ll packet_address;
    socklen_t address_len = sizeof(packet_address);
    ssize_t packet_len = recvfrom(priv->packet_fd, packet, sizeof(packet),
                                  MSG_TRUNC, (struct sockaddr *)&packet_address,
                                  &address_len);
    if (packet_len < 0) {
        if (errno == EINTR || errno == EAGAIN) return PS_ERR_TIMEOUT;
        return PS_ERR_SCANNER_FAILED;
    }
    if (packet_address.sll_pkttype == PACKET_OUTGOING) return PS_ERR_TIMEOUT;
    if ((size_t)packet_len > sizeof(packet)) return PS_ERR_MALFORMED;
    return ps_wifi_parse_radiotap_probe_request(packet, (size_t)packet_len, out);
}

static void wifi_close(ps_scanner_t *scanner)
{
    if (!scanner || !scanner->priv) return;
    struct wifi_priv *priv = scanner->priv;
    if (priv->packet_fd >= 0) {
        close(priv->packet_fd);
        priv->packet_fd = -1;
    }
    if (priv->brought_iface_up && !priv->owns_monitor_interface) {
        int control_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (control_fd >= 0) {
            struct ifreq request;
            memset(&request, 0, sizeof(request));
            (void)strncpy(request.ifr_name, priv->monitor_iface,
                          sizeof(request.ifr_name) - 1U);
            if (ioctl(control_fd, SIOCGIFFLAGS, &request) == 0) {
                request.ifr_flags = (short)(request.ifr_flags & (short)~IFF_UP);
                (void)ioctl(control_fd, SIOCSIFFLAGS, &request);
            }
            close(control_fd);
        }
        priv->brought_iface_up = false;
    }
    delete_monitor_interface(priv);
    if (priv->netlink_fd >= 0) {
        close(priv->netlink_fd);
        priv->netlink_fd = -1;
    }
    scanner->is_open = false;
    free(priv);
    scanner->priv = NULL;
}

static const char *wifi_describe(const ps_scanner_t *scanner)
{
    if (!scanner || !scanner->priv) return "Wi-Fi nl80211 monitor-mode scanner";
    const struct wifi_priv *priv = scanner->priv;
    return priv->config.iface[0] == '\0' ?
           "Wi-Fi nl80211 monitor-mode scanner (automatic interface)" :
           "Wi-Fi nl80211 monitor-mode scanner (selected interface)";
}
