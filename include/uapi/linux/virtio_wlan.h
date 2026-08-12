/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Virtio WLAN device: a cfg80211-level (fullmac-style) WLAN device.
 *
 * The device implements association: the driver forwards cfg80211
 * requests (scan, connect, keys) as commands and receives results as
 * events. Frames on the data queues are 802.3, so EAPOL is handled by
 * the driver side's ordinary supplicant and keys are installed with
 * VIRTIO_WLAN_CMD_ADD_KEY.
 *
 * All multi-byte fields are little-endian.
 *
 * Ordering rule: the device must complete a command before emitting
 * any event that reports the outcome of that command. A driver is
 * entitled to discard a VIRTIO_WLAN_EV_SCAN_DONE or
 * VIRTIO_WLAN_EV_CONNECT_RESULT that arrives before the corresponding
 * VIRTIO_WLAN_CMD_SCAN or VIRTIO_WLAN_CMD_CONNECT has been answered,
 * because until then it cannot know the request was accepted.
 */
#ifndef _LINUX_VIRTIO_WLAN_H
#define _LINUX_VIRTIO_WLAN_H

#include <linux/types.h>
#include <linux/virtio_types.h>

/*
 * Virtqueues. The data queues carry bare 802.3 frames, one frame per
 * buffer, with no per-buffer header; a future feature bit may add one
 * to carry offload metadata.
 */
#define VIRTIO_WLAN_VQ_RX		0
#define VIRTIO_WLAN_VQ_TX		1
#define VIRTIO_WLAN_VQ_CMD		2
#define VIRTIO_WLAN_VQ_EVENT		3
#define VIRTIO_WLAN_VQ_MAX		4

/* Feature bits */
#define VIRTIO_WLAN_F_MAC		0	/* config.mac is valid */

struct virtio_wlan_config {
	__u8 mac[6];
};

/* Protocol limits. */
#define VIRTIO_WLAN_MAX_SSID_LEN	32
#define VIRTIO_WLAN_MAX_KEY_LEN		32
#define VIRTIO_WLAN_MAX_SEQ_LEN		16
#define VIRTIO_WLAN_MAX_CIPHERS		5
#define VIRTIO_WLAN_MAX_AKMS		10

/* Bands. Deliberately independent of the nl80211 enum. */
#define VIRTIO_WLAN_BAND_2GHZ		0
#define VIRTIO_WLAN_BAND_5GHZ		1
#define VIRTIO_WLAN_BAND_6GHZ		2

/* Channel flags. */
#define VIRTIO_WLAN_CHAN_F_DISABLED	(1 << 0)
#define VIRTIO_WLAN_CHAN_F_NO_IR	(1 << 1)
#define VIRTIO_WLAN_CHAN_F_RADAR	(1 << 2)
#define VIRTIO_WLAN_CHAN_F_NO_HT40PLUS	(1 << 3)
#define VIRTIO_WLAN_CHAN_F_NO_HT40MINUS	(1 << 4)
#define VIRTIO_WLAN_CHAN_F_NO_80MHZ	(1 << 5)
#define VIRTIO_WLAN_CHAN_F_NO_160MHZ	(1 << 6)

/*
 * Commands. Each command occupies one descriptor chain on the command
 * virtqueue: a device-readable part (struct virtio_wlan_cmd_hdr plus
 * the command payload) followed by a device-writable part (struct
 * virtio_wlan_cmd_resp plus the response payload, if any).
 */
#define VIRTIO_WLAN_CMD_GET_WIPHY	0
#define VIRTIO_WLAN_CMD_SCAN		1
#define VIRTIO_WLAN_CMD_ABORT_SCAN	2
#define VIRTIO_WLAN_CMD_CONNECT		3
#define VIRTIO_WLAN_CMD_DISCONNECT	4
#define VIRTIO_WLAN_CMD_ADD_KEY		5
#define VIRTIO_WLAN_CMD_DEL_KEY		6
#define VIRTIO_WLAN_CMD_SET_DEFAULT_KEY	7
#define VIRTIO_WLAN_CMD_GET_STATION	8
/* Management-frame key (IGTK), used when management frame protection
 * is negotiated. */
#define VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY	9
/*
 * Open the 802.1X controlled port for a station. The driver side keeps
 * the port closed to ordinary data until the supplicant -- which runs
 * on the driver-facing side, since EAPOL crosses the data queues --
 * reports the handshake complete.
 */
#define VIRTIO_WLAN_CMD_SET_PORT_AUTHORIZED	10

/* Command status. */
#define VIRTIO_WLAN_S_OK		0
#define VIRTIO_WLAN_S_EINVAL		1
#define VIRTIO_WLAN_S_ENOTSUPP		2
#define VIRTIO_WLAN_S_EBUSY		3
#define VIRTIO_WLAN_S_EIO		4
#define VIRTIO_WLAN_S_ENOENT		5
#define VIRTIO_WLAN_S_ENOMEM		6

/*
 * Both headers are eight bytes so that the payload that follows is
 * always 64-bit aligned and can be accessed in place.
 */
struct virtio_wlan_cmd_hdr {
	__le16 cmd;
	__le16 flags;			/* MBZ */
	__le32 reserved;
};

struct virtio_wlan_cmd_resp {
	__le16 status;
	__le16 reserved;
	__le32 reserved2;
};

/*
 * VIRTIO_WLAN_CMD_GET_WIPHY: no payload. The response payload is a
 * struct virtio_wlan_wiphy_info followed by n_cipher_suites __le32
 * cipher suite selectors, then n_channels struct virtio_wlan_channel,
 * then tlv_len bytes reserved for future typed attributes (a driver
 * that does not understand them ignores them).
 */
struct virtio_wlan_wiphy_info {
	__le32 max_scan_ssids;
	__le32 max_scan_ie_len;
	__le32 max_num_pmkids;
	__le32 n_cipher_suites;
	__le32 n_channels;
	__le32 flags;			/* MBZ */
	__le32 tlv_len;
};

struct virtio_wlan_channel {
	__le32 center_freq;		/* MHz */
	__le32 flags;			/* VIRTIO_WLAN_CHAN_F_* */
	__le32 max_power;		/* dBm */
	__u8 band;			/* VIRTIO_WLAN_BAND_* */
	__u8 reserved[3];
};

struct virtio_wlan_ssid {
	__u8 ssid_len;
	__u8 ssid[VIRTIO_WLAN_MAX_SSID_LEN];
	__u8 reserved[3];
};

/*
 * VIRTIO_WLAN_CMD_SCAN: the payload is followed by n_ssids struct
 * virtio_wlan_ssid, then n_channels __le32 centre frequencies (an
 * empty list means all supported channels), then ie_len bytes of
 * extra information elements to include in probe requests.
 */
struct virtio_wlan_cmd_scan {
	__le32 n_ssids;
	__le32 n_channels;
	__le32 ie_len;
	__le32 flags;			/* MBZ */
};

/*
 * VIRTIO_WLAN_CMD_CONNECT: the payload is followed by ie_len bytes of
 * association request information elements. The response only reports
 * whether the request was accepted; the outcome arrives as a
 * VIRTIO_WLAN_EV_CONNECT_RESULT event.
 */
struct virtio_wlan_cmd_connect {
	__u8 bssid[6];
	__u8 bssid_valid;
	__u8 ssid_len;
	__u8 ssid[VIRTIO_WLAN_MAX_SSID_LEN];
	__u8 privacy;
	__u8 want_1x;
	__u8 reserved[2];
	__le32 center_freq;		/* 0: unspecified */
	__le32 auth_type;		/* NL80211_AUTHTYPE_* */
	__le32 mfp;			/* NL80211_MFP_* */
	__le32 wpa_versions;
	__le32 cipher_group;
	__le32 n_ciphers_pairwise;
	__le32 ciphers_pairwise[VIRTIO_WLAN_MAX_CIPHERS];
	__le32 n_akm_suites;
	__le32 akm_suites[VIRTIO_WLAN_MAX_AKMS];
	__le32 ie_len;
};

struct virtio_wlan_cmd_disconnect {
	__le16 reason_code;
	__u8 reserved[2];
};

struct virtio_wlan_cmd_add_key {
	__u8 key_idx;
	__u8 pairwise;
	__u8 mac[6];
	__u8 mac_valid;
	__u8 key_len;
	__u8 seq_len;
	__u8 reserved;
	__le32 cipher;
	__u8 key[VIRTIO_WLAN_MAX_KEY_LEN];
	__u8 seq[VIRTIO_WLAN_MAX_SEQ_LEN];
};

struct virtio_wlan_cmd_del_key {
	__u8 key_idx;
	__u8 pairwise;
	__u8 mac[6];
	__u8 mac_valid;
	__u8 reserved[3];
};

struct virtio_wlan_cmd_set_default_key {
	__u8 key_idx;
	__u8 unicast;
	__u8 multicast;
	__u8 reserved;
};

struct virtio_wlan_cmd_set_default_mgmt_key {
	__u8 key_idx;
	__u8 reserved[3];
};

struct virtio_wlan_cmd_set_port_authorized {
	__u8 mac[6];
	__u8 authorized;
	__u8 reserved;
};

/* VIRTIO_WLAN_CMD_GET_STATION: request is the station address. */
struct virtio_wlan_cmd_get_station {
	__u8 mac[6];
	__u8 reserved[2];
};

/* Which fields of struct virtio_wlan_station_info are valid. */
#define VIRTIO_WLAN_STA_F_SIGNAL	(1 << 0)
#define VIRTIO_WLAN_STA_F_TX_BITRATE	(1 << 1)
#define VIRTIO_WLAN_STA_F_RX_BITRATE	(1 << 2)
#define VIRTIO_WLAN_STA_F_COUNTERS	(1 << 3)
#define VIRTIO_WLAN_STA_F_CONNECTED_TIME (1 << 4)

struct virtio_wlan_station_info {
	__le32 filled;			/* VIRTIO_WLAN_STA_F_* */
	__le32 signal;			/* s32, dBm */
	__le32 tx_bitrate;		/* 100 kbit/s units */
	__le32 rx_bitrate;		/* 100 kbit/s units */
	__le32 tx_failed;
	__le32 connected_time;		/* seconds */
	__le64 rx_packets;
	__le64 tx_packets;
	__le64 rx_bytes;
	__le64 tx_bytes;
};

/*
 * Events. The driver posts device-writable buffers on the event
 * virtqueue; the device fills one event per buffer, starting with
 * struct virtio_wlan_event_hdr.
 */
#define VIRTIO_WLAN_EV_SCAN_RESULT	0
#define VIRTIO_WLAN_EV_SCAN_DONE	1
#define VIRTIO_WLAN_EV_CONNECT_RESULT	2
#define VIRTIO_WLAN_EV_DISCONNECTED	3
#define VIRTIO_WLAN_EV_MIC_FAILURE	4

/* Eight bytes, so event payloads are 64-bit aligned in the buffer. */
struct virtio_wlan_event_hdr {
	__le16 type;
	__le16 len;			/* payload bytes after this header */
	__le32 reserved;
};

/* Frame the scan result was derived from. */
#define VIRTIO_WLAN_FTYPE_UNKNOWN	0
#define VIRTIO_WLAN_FTYPE_BEACON	1
#define VIRTIO_WLAN_FTYPE_PRESP		2

/*
 * Followed by ie_len bytes of raw information elements. The TSF comes
 * first so that it stays 64-bit aligned within the event buffer.
 */
struct virtio_wlan_ev_scan_result {
	__le64 tsf;
	__u8 bssid[6];
	__u8 ftype;			/* VIRTIO_WLAN_FTYPE_* */
	__u8 reserved;
	__le16 capability;
	__le16 beacon_interval;		/* TU */
	__le32 center_freq;		/* MHz */
	__le32 signal;			/* s32, mBm */
	__le32 ie_len;
};

struct virtio_wlan_ev_scan_done {
	__u8 aborted;
	__u8 reserved[3];
};

/* Followed by req_ie_len then resp_ie_len bytes of information elements. */
struct virtio_wlan_ev_connect_result {
	__u8 bssid[6];
	__le16 status;			/* 0: success, else WLAN_STATUS_* */
	__le32 req_ie_len;
	__le32 resp_ie_len;
};

/* Followed by ie_len bytes of information elements. */
struct virtio_wlan_ev_disconnected {
	__le16 reason;
	__u8 from_ap;
	__u8 reserved;
	__le32 ie_len;
};

struct virtio_wlan_ev_mic_failure {
	__u8 mac[6];
	__u8 key_idx;
	__u8 pairwise;
};

#endif /* _LINUX_VIRTIO_WLAN_H */
