/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef _UAPI_LINUX_VIRTIO_WLAN_H
#define _UAPI_LINUX_VIRTIO_WLAN_H

#include <linux/types.h>

/*
 * A virtio-wlan device is a FullMAC station. The rx and tx queues carry
 * Ethernet frames, each behind a struct virtio_net_hdr_v1. Every chain on the
 * command queue holds a command header and its payload, followed by a
 * writable response header and its payload. Every buffer on the event queue
 * receives one event header and its payload.
 *
 * All fields are little-endian, frequencies are in MHz, and signals are
 * signed values in two's complement. Cipher and AKM suites are 32-bit values
 * with the OUI in the upper 24 bits, as in WLAN_CIPHER_SUITE_CCMP. An
 * all-zero peer or BSSID means that there is none.
 */

#define VIRTIO_WLAN_F_TX_CSUM		0
#define VIRTIO_WLAN_F_TX_GSO		1
#define VIRTIO_WLAN_F_RX_CSUM		2
#define VIRTIO_WLAN_F_RX_GSO		3
#define VIRTIO_WLAN_F_MRG_RXBUF		4
#define VIRTIO_WLAN_F_EXTERNAL_AUTH	5
#define VIRTIO_WLAN_F_SET_MAC		6

#define VIRTIO_WLAN_VQ_RX		0
#define VIRTIO_WLAN_VQ_TX		1
#define VIRTIO_WLAN_VQ_CMD		2
#define VIRTIO_WLAN_VQ_EVENT		3
#define VIRTIO_WLAN_VQ_MAX		4

#define VIRTIO_WLAN_SSID_LEN		32
#define VIRTIO_WLAN_MAX_CIPHER_SUITES	16
#define VIRTIO_WLAN_MAX_PAIRWISE	5
#define VIRTIO_WLAN_MAX_AKM_SUITES	10
#define VIRTIO_WLAN_KEY_LEN		32
#define VIRTIO_WLAN_SEQ_LEN		16
#define VIRTIO_WLAN_WEP_KEY_LEN		13
#define VIRTIO_WLAN_HE_PPE_LEN		25

struct virtio_wlan_config {
	__u8 mac[6];
	__le16 mtu;
	__le32 max_event_size;
	__le32 max_wiphy_size;
};

enum virtio_wlan_cmd_type {
	VIRTIO_WLAN_CMD_GET_WIPHY = 1,
	VIRTIO_WLAN_CMD_OPEN,
	VIRTIO_WLAN_CMD_CLOSE,
	VIRTIO_WLAN_CMD_SCAN,
	VIRTIO_WLAN_CMD_ABORT_SCAN,
	VIRTIO_WLAN_CMD_CONNECT,
	VIRTIO_WLAN_CMD_DISCONNECT,
	VIRTIO_WLAN_CMD_ADD_KEY,
	VIRTIO_WLAN_CMD_DEL_KEY,
	VIRTIO_WLAN_CMD_SET_DEFAULT_KEY,
	VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY,
	VIRTIO_WLAN_CMD_AUTHORIZE,
	VIRTIO_WLAN_CMD_GET_STATION,
	VIRTIO_WLAN_CMD_SET_POWER_MGMT,
	VIRTIO_WLAN_CMD_SET_MAC,
	VIRTIO_WLAN_CMD_SET_COUNTRY,
	VIRTIO_WLAN_CMD_EXTERNAL_AUTH,
	VIRTIO_WLAN_CMD_MGMT_TX,
	VIRTIO_WLAN_CMD_SET_CQM_RSSI,
	VIRTIO_WLAN_CMD_SET_WIPHY_PARAMS,
	VIRTIO_WLAN_CMD_SET_TX_POWER,
};

enum virtio_wlan_status {
	VIRTIO_WLAN_S_OK = 0,
	VIRTIO_WLAN_S_FAILED,
	VIRTIO_WLAN_S_INVALID,
	VIRTIO_WLAN_S_UNSUPPORTED,
	VIRTIO_WLAN_S_BUSY,
	VIRTIO_WLAN_S_ALREADY,
	VIRTIO_WLAN_S_NETDOWN,
	VIRTIO_WLAN_S_NOT_FOUND,
	VIRTIO_WLAN_S_NOT_CONNECTED,
	VIRTIO_WLAN_S_NO_MEMORY,
	VIRTIO_WLAN_S_NO_DEVICE,
};

enum virtio_wlan_event_type {
	VIRTIO_WLAN_EVENT_BSS = 1,
	VIRTIO_WLAN_EVENT_SCAN_DONE,
	VIRTIO_WLAN_EVENT_CONNECT_RESULT,
	VIRTIO_WLAN_EVENT_DISCONNECTED,
	VIRTIO_WLAN_EVENT_MIC_FAILURE,
	VIRTIO_WLAN_EVENT_MGMT_RX,
	VIRTIO_WLAN_EVENT_EXTERNAL_AUTH_REQ,
	VIRTIO_WLAN_EVENT_COUNTRY,
	VIRTIO_WLAN_EVENT_CQM_RSSI,
};

/* The flags of the command and event headers are zero. */
struct virtio_wlan_cmd_hdr {
	__le16 type;
	__le16 flags;
	__le32 len;
};

struct virtio_wlan_resp {
	__le16 status;
	__le16 reserved;
	__le32 len;
};

struct virtio_wlan_event_hdr {
	__le16 type;
	__le16 flags;
	__le32 len;
};

#define VIRTIO_WLAN_WIPHY_F_SCAN_RANDOM_MAC	(1 << 0)
#define VIRTIO_WLAN_WIPHY_F_MFP_OPTIONAL	(1 << 1)
#define VIRTIO_WLAN_WIPHY_F_CAN_REPLACE_PTK0	(1 << 2)

enum virtio_wlan_band_id {
	VIRTIO_WLAN_BAND_2GHZ = 0,
	VIRTIO_WLAN_BAND_5GHZ,
	VIRTIO_WLAN_BAND_6GHZ,
};

#define VIRTIO_WLAN_BAND_F_HT		(1 << 0)
#define VIRTIO_WLAN_BAND_F_VHT		(1 << 1)
#define VIRTIO_WLAN_BAND_F_HE		(1 << 2)
#define VIRTIO_WLAN_BAND_F_HE_6GHZ	(1 << 3)

#define VIRTIO_WLAN_CHAN_DISABLED	(1 << 0)
#define VIRTIO_WLAN_CHAN_NO_IR		(1 << 1)
#define VIRTIO_WLAN_CHAN_RADAR		(1 << 2)
#define VIRTIO_WLAN_CHAN_NO_HT40PLUS	(1 << 3)
#define VIRTIO_WLAN_CHAN_NO_HT40MINUS	(1 << 4)
#define VIRTIO_WLAN_CHAN_NO_OFDM	(1 << 5)
#define VIRTIO_WLAN_CHAN_NO_80MHZ	(1 << 6)
#define VIRTIO_WLAN_CHAN_NO_160MHZ	(1 << 7)
#define VIRTIO_WLAN_CHAN_INDOOR_ONLY	(1 << 8)
#define VIRTIO_WLAN_CHAN_IR_CONCURRENT	(1 << 9)
#define VIRTIO_WLAN_CHAN_NO_20MHZ	(1 << 10)
#define VIRTIO_WLAN_CHAN_NO_10MHZ	(1 << 11)
#define VIRTIO_WLAN_CHAN_NO_HE		(1 << 12)

/* The maximum power is in dBm. */
struct virtio_wlan_channel {
	__le16 freq;
	__le16 max_power;
	__le32 flags;
};

/* The fields follow struct ieee80211_sta_ht_cap. */
struct virtio_wlan_ht_cap {
	__le16 cap;
	__u8 ampdu_factor;
	__u8 ampdu_density;
	__u8 rx_mask[10];
	__le16 rx_highest;
	__u8 tx_params;
	__u8 reserved[7];
};

/* The fields follow struct ieee80211_sta_vht_cap. */
struct virtio_wlan_vht_cap {
	__le32 cap;
	__le16 rx_mcs_map;
	__le16 rx_highest;
	__le16 tx_mcs_map;
	__le16 tx_highest;
	__le32 reserved;
};

/* The fields follow struct ieee80211_sta_he_cap. */
struct virtio_wlan_he_cap {
	__u8 mac_cap[6];
	__u8 phy_cap[11];
	__u8 reserved;
	__le16 rx_mcs_80;
	__le16 tx_mcs_80;
	__le16 rx_mcs_160;
	__le16 tx_mcs_160;
	__le16 rx_mcs_80p80;
	__le16 tx_mcs_80p80;
	__u8 ppe_thres[VIRTIO_WLAN_HE_PPE_LEN];
	__u8 reserved2;
};

/*
 * A band is followed by n_channels struct virtio_wlan_channel. The flags
 * say which capabilities are present.
 */
struct virtio_wlan_band {
	__u8 band;
	__u8 flags;
	__le16 n_channels;
	__le16 he_6ghz_capa;
	__le16 reserved;
	struct virtio_wlan_ht_cap ht;
	struct virtio_wlan_vht_cap vht;
	struct virtio_wlan_he_cap he;
};

/*
 * The response to GET_WIPHY is a struct virtio_wlan_wiphy followed by n_bands
 * bands. max_akm_suites is the largest number of AKM suites that CONNECT may
 * carry.
 */
struct virtio_wlan_wiphy {
	__le32 flags;
	__le16 max_scan_ie_len;
	__u8 max_scan_ssids;
	__u8 n_bands;
	__u8 n_cipher_suites;
	__u8 max_akm_suites;
	__u8 reserved[6];
	__le32 cipher_suites[VIRTIO_WLAN_MAX_CIPHER_SUITES];
};

#define VIRTIO_WLAN_SCAN_F_RANDOM_MAC	(1 << 0)

struct virtio_wlan_ssid {
	__u8 len;
	__u8 ssid[VIRTIO_WLAN_SSID_LEN];
	__u8 reserved[7];
};

/*
 * A scan is followed by n_ssids struct virtio_wlan_ssid, n_freqs __le16
 * frequencies and ie_len bytes of IEs. A scan without frequencies covers
 * every enabled channel.
 */
struct virtio_wlan_scan {
	__le32 id;
	__le32 flags;
	__u8 mac_addr[6];
	__u8 mac_addr_mask[6];
	__le16 n_ssids;
	__le16 n_freqs;
	__le16 ie_len;
	__u8 bssid[6];
};

struct virtio_wlan_abort_scan {
	__le32 id;
	__le32 reserved;
};

enum virtio_wlan_auth_type {
	VIRTIO_WLAN_AUTH_OPEN = 0,
	VIRTIO_WLAN_AUTH_SHARED_KEY,
	VIRTIO_WLAN_AUTH_SAE,
	VIRTIO_WLAN_AUTH_AUTOMATIC,
};

enum virtio_wlan_mfp {
	VIRTIO_WLAN_MFP_NO = 0,
	VIRTIO_WLAN_MFP_REQUIRED,
	VIRTIO_WLAN_MFP_OPTIONAL,
};

#define VIRTIO_WLAN_WPA_VERSION_1	(1 << 0)
#define VIRTIO_WLAN_WPA_VERSION_2	(1 << 1)
#define VIRTIO_WLAN_WPA_VERSION_3	(1 << 2)

#define VIRTIO_WLAN_CONNECT_F_PRIVACY			(1 << 0)
#define VIRTIO_WLAN_CONNECT_F_CONTROL_PORT		(1 << 1)
#define VIRTIO_WLAN_CONNECT_F_CONTROL_PORT_NO_ENCRYPT	(1 << 2)
#define VIRTIO_WLAN_CONNECT_F_DISABLE_HT		(1 << 3)
#define VIRTIO_WLAN_CONNECT_F_DISABLE_VHT		(1 << 4)
#define VIRTIO_WLAN_CONNECT_F_DISABLE_HE		(1 << 5)
#define VIRTIO_WLAN_CONNECT_F_DISABLE_EHT		(1 << 6)

/*
 * A connection request is followed by ie_len bytes of IEs. A zero frequency
 * or frequency hint means that none is given. The control port ethertype is
 * a number, such as 0x888e for EAPOL. A nonzero key_len gives a static WEP
 * key, which Shared Key authentication needs.
 */
struct virtio_wlan_connect {
	__le32 id;
	__le32 flags;
	__le32 wpa_versions;
	__le32 cipher_group;
	__le32 ciphers_pairwise[VIRTIO_WLAN_MAX_PAIRWISE];
	__le32 akm_suites[VIRTIO_WLAN_MAX_AKM_SUITES];
	__u8 bssid[6];
	__u8 bssid_hint[6];
	__le16 freq;
	__le16 freq_hint;
	__le16 control_port_ethertype;
	__le16 ie_len;
	__u8 ssid_len;
	__u8 auth_type;
	__u8 mfp;
	__u8 n_ciphers_pairwise;
	__u8 n_akm_suites;
	__u8 key_idx;
	__u8 key_len;
	__u8 reserved;
	__u8 ssid[VIRTIO_WLAN_SSID_LEN];
	__u8 key[VIRTIO_WLAN_WEP_KEY_LEN];
	__u8 reserved2[3];
};

struct virtio_wlan_disconnect {
	__le32 id;
	__le16 reason;
	__le16 reserved;
};

#define VIRTIO_WLAN_KEY_F_PAIRWISE	(1 << 0)

struct virtio_wlan_add_key {
	__le32 cipher;
	__u8 index;
	__u8 flags;
	__u8 key_len;
	__u8 seq_len;
	__u8 peer[6];
	__u8 reserved[2];
	__u8 key[VIRTIO_WLAN_KEY_LEN];
	__u8 seq[VIRTIO_WLAN_SEQ_LEN];
};

struct virtio_wlan_del_key {
	__u8 index;
	__u8 flags;
	__u8 peer[6];
};

#define VIRTIO_WLAN_DEFAULT_KEY_F_UNICAST	(1 << 0)
#define VIRTIO_WLAN_DEFAULT_KEY_F_MULTICAST	(1 << 1)

struct virtio_wlan_set_default_key {
	__u8 index;
	__u8 flags;
	__u8 reserved[6];
};

struct virtio_wlan_set_default_mgmt_key {
	__u8 index;
	__u8 reserved[7];
};

struct virtio_wlan_authorize {
	__u8 peer[6];
	__u8 authorized;
	__u8 reserved;
};

struct virtio_wlan_get_station {
	__u8 peer[6];
	__u8 reserved[2];
};

enum virtio_wlan_rate_type {
	VIRTIO_WLAN_RATE_LEGACY = 0,
	VIRTIO_WLAN_RATE_HT,
	VIRTIO_WLAN_RATE_VHT,
	VIRTIO_WLAN_RATE_HE,
};

enum virtio_wlan_rate_bw {
	VIRTIO_WLAN_RATE_BW_20 = 0,
	VIRTIO_WLAN_RATE_BW_5,
	VIRTIO_WLAN_RATE_BW_10,
	VIRTIO_WLAN_RATE_BW_40,
	VIRTIO_WLAN_RATE_BW_80,
	VIRTIO_WLAN_RATE_BW_160,
	VIRTIO_WLAN_RATE_BW_HE_RU,
};

#define VIRTIO_WLAN_RATE_F_SHORT_GI	(1 << 0)

/*
 * The legacy rate is in units of 100 kb/s. For HT, mcs is the HT MCS index,
 * which includes the number of spatial streams, and nss is ignored. For VHT
 * and HE, mcs is the index for one stream and nss the number of streams. The
 * HE fields follow struct rate_info.
 */
struct virtio_wlan_rate {
	__u8 type;
	__u8 bw;
	__u8 mcs;
	__u8 nss;
	__u8 flags;
	__u8 he_gi;
	__u8 he_dcm;
	__u8 he_ru_alloc;
	__le16 legacy;
	__le16 reserved;
};

#define VIRTIO_WLAN_STA_CONNECTED_TIME	(1 << 0)
#define VIRTIO_WLAN_STA_INACTIVE_TIME	(1 << 1)
#define VIRTIO_WLAN_STA_RX_BYTES	(1 << 2)
#define VIRTIO_WLAN_STA_TX_BYTES	(1 << 3)
#define VIRTIO_WLAN_STA_RX_PACKETS	(1 << 4)
#define VIRTIO_WLAN_STA_TX_PACKETS	(1 << 5)
#define VIRTIO_WLAN_STA_TX_RETRIES	(1 << 6)
#define VIRTIO_WLAN_STA_TX_FAILED	(1 << 7)
#define VIRTIO_WLAN_STA_SIGNAL		(1 << 8)
#define VIRTIO_WLAN_STA_SIGNAL_AVG	(1 << 9)
#define VIRTIO_WLAN_STA_TX_RATE		(1 << 10)
#define VIRTIO_WLAN_STA_RX_RATE		(1 << 11)

/*
 * GET_STATION responds with this structure, whose filled bits say which
 * fields are valid. The connected time is in seconds, the inactive time in
 * milliseconds and the signals in dBm.
 */
struct virtio_wlan_station_info {
	__le32 filled;
	__le32 connected_time;
	__le32 inactive_time;
	__le32 rx_packets;
	__le32 tx_packets;
	__le32 tx_retries;
	__le32 tx_failed;
	__s8 signal;
	__s8 signal_avg;
	__u8 reserved[2];
	__le64 rx_bytes;
	__le64 tx_bytes;
	struct virtio_wlan_rate tx_rate;
	struct virtio_wlan_rate rx_rate;
};

#define VIRTIO_WLAN_PS_TIMEOUT_DEFAULT	0xffffffff

/* The timeout is in milliseconds. */
struct virtio_wlan_set_power_mgmt {
	__u8 enabled;
	__u8 reserved[3];
	__le32 timeout;
};

struct virtio_wlan_set_mac {
	__u8 addr[6];
	__u8 reserved[2];
};

struct virtio_wlan_set_country {
	__u8 alpha2[2];
	__u8 reserved[6];
};

/* The status is an IEEE 802.11 status code. */
struct virtio_wlan_external_auth {
	__le32 id;
	__le16 status;
	__u8 bssid[6];
	__u8 reserved[4];
};

/* An Authentication frame of len bytes follows. */
struct virtio_wlan_mgmt_tx {
	__le32 id;
	__le16 freq;
	__le16 len;
};

enum virtio_wlan_frame_type {
	VIRTIO_WLAN_FRAME_BEACON = 0,
	VIRTIO_WLAN_FRAME_PROBE_RESP,
};

/*
 * BSS and CONNECT_RESULT events describe a BSS with this structure, followed
 * by ie_len bytes of IEs. The signal is in mBm.
 */
struct virtio_wlan_bss_info {
	__le64 tsf;
	__le32 signal;
	__le16 freq;
	__le16 capability;
	__le16 beacon_interval;
	__u8 bssid[6];
	__u8 frame_type;
	__u8 reserved;
	__le16 ie_len;
	__le32 reserved2;
};

struct virtio_wlan_event_bss {
	__le32 id;
	__le32 reserved;
	struct virtio_wlan_bss_info bss;
};

struct virtio_wlan_event_scan_done {
	__le32 id;
	__u8 aborted;
	__u8 reserved[3];
};

enum virtio_wlan_timeout_reason {
	VIRTIO_WLAN_TIMEOUT_NONE = 0,
	VIRTIO_WLAN_TIMEOUT_UNSPECIFIED,
	VIRTIO_WLAN_TIMEOUT_SCAN,
	VIRTIO_WLAN_TIMEOUT_AUTH,
	VIRTIO_WLAN_TIMEOUT_ASSOC,
};

/*
 * A connection result is followed by the BSS's IEs, req_ie_len bytes of
 * request IEs and resp_ie_len bytes of response IEs. The status is an IEEE
 * 802.11 status code. A nonzero timeout_reason marks a failure without a
 * status code, which cfg80211 reports as status -1, and the status is then
 * ignored. A failed attempt may have no BSS, which is an all-zero bssid with a
 * zero frequency and no IEs. A successful attempt always has one.
 */
struct virtio_wlan_event_connect_result {
	__le32 id;
	__le16 status;
	__le16 req_ie_len;
	__le16 resp_ie_len;
	__u8 timeout_reason;
	__u8 reserved[5];
	struct virtio_wlan_bss_info bss;
};

/* A disconnection is followed by ie_len bytes of IEs. */
struct virtio_wlan_event_disconnected {
	__le32 id;
	__le16 reason;
	__le16 ie_len;
	__u8 locally_generated;
	__u8 reserved[7];
};

#define VIRTIO_WLAN_KEY_INDEX_NONE	0xff

/* The key index is VIRTIO_WLAN_KEY_INDEX_NONE when the key is unknown. */
struct virtio_wlan_event_mic_failure {
	__u8 peer[6];
	__u8 key_index;
	__u8 pairwise;
};

/* An Authentication frame of len bytes follows. The signal is in mBm. */
struct virtio_wlan_event_mgmt_rx {
	__le32 id;
	__le32 signal;
	__le16 freq;
	__le16 len;
	__le32 reserved;
};

struct virtio_wlan_event_external_auth_req {
	__le32 id;
	__le32 akm;
	__u8 bssid[6];
	__u8 ssid_len;
	__u8 reserved;
	__u8 ssid[VIRTIO_WLAN_SSID_LEN];
};

/* A country is followed by n_channels struct virtio_wlan_channel. */
struct virtio_wlan_event_country {
	__u8 alpha2[2];
	__le16 n_channels;
	__le32 reserved;
};

#ifdef __KERNEL__
#include <linux/build_bug.h>
#include <linux/stddef.h>

static_assert(sizeof(struct virtio_wlan_config) == 16);
static_assert(sizeof(struct virtio_wlan_cmd_hdr) == 8);
static_assert(sizeof(struct virtio_wlan_resp) == 8);
static_assert(sizeof(struct virtio_wlan_event_hdr) == 8);
static_assert(sizeof(struct virtio_wlan_channel) == 8);
static_assert(sizeof(struct virtio_wlan_ht_cap) == 24);
static_assert(sizeof(struct virtio_wlan_vht_cap) == 16);
static_assert(sizeof(struct virtio_wlan_he_cap) == 56);
static_assert(sizeof(struct virtio_wlan_band) == 104);
static_assert(sizeof(struct virtio_wlan_wiphy) == 80);
static_assert(sizeof(struct virtio_wlan_ssid) == 40);
static_assert(sizeof(struct virtio_wlan_scan) == 32);
static_assert(sizeof(struct virtio_wlan_abort_scan) == 8);
static_assert(sizeof(struct virtio_wlan_connect) == 152);
static_assert(sizeof(struct virtio_wlan_disconnect) == 8);
static_assert(sizeof(struct virtio_wlan_add_key) == 64);
static_assert(sizeof(struct virtio_wlan_del_key) == 8);
static_assert(sizeof(struct virtio_wlan_set_default_key) == 8);
static_assert(sizeof(struct virtio_wlan_set_default_mgmt_key) == 8);
static_assert(sizeof(struct virtio_wlan_authorize) == 8);
static_assert(sizeof(struct virtio_wlan_get_station) == 8);
static_assert(sizeof(struct virtio_wlan_rate) == 12);
static_assert(sizeof(struct virtio_wlan_station_info) == 72);
static_assert(sizeof(struct virtio_wlan_set_power_mgmt) == 8);
static_assert(sizeof(struct virtio_wlan_set_mac) == 8);
static_assert(sizeof(struct virtio_wlan_set_country) == 8);
static_assert(sizeof(struct virtio_wlan_external_auth) == 16);
static_assert(sizeof(struct virtio_wlan_mgmt_tx) == 8);
static_assert(sizeof(struct virtio_wlan_bss_info) == 32);
static_assert(sizeof(struct virtio_wlan_event_bss) == 40);
static_assert(sizeof(struct virtio_wlan_event_scan_done) == 8);
static_assert(sizeof(struct virtio_wlan_event_connect_result) == 48);
static_assert(sizeof(struct virtio_wlan_event_disconnected) == 16);
static_assert(sizeof(struct virtio_wlan_event_mic_failure) == 8);
static_assert(sizeof(struct virtio_wlan_event_mgmt_rx) == 16);
static_assert(sizeof(struct virtio_wlan_event_external_auth_req) == 48);
static_assert(sizeof(struct virtio_wlan_event_country) == 8);

static_assert(offsetof(struct virtio_wlan_station_info, rx_bytes) % 8 == 0);
static_assert(offsetof(struct virtio_wlan_station_info, tx_bytes) % 8 == 0);
static_assert(offsetof(struct virtio_wlan_bss_info, tsf) % 8 == 0);
static_assert(offsetof(struct virtio_wlan_event_bss, bss) % 8 == 0);
static_assert(offsetof(struct virtio_wlan_event_connect_result, bss) % 8 == 0);
#endif

#endif
