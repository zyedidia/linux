// SPDX-License-Identifier: GPL-2.0
/*
 * A driver for virtio-wlan devices, FullMAC stations whose wire format is
 * defined in include/uapi/linux/virtio_wlan.h. The device is untrusted:
 * everything it sends is checked before it reaches cfg80211.
 */

#include <linux/debugfs.h>
#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_net.h>
#include <linux/virtio_wlan.h>
#include <net/cfg80211.h>

#define VIRTIO_WLAN_CMD_TIMEOUT		(5 * HZ)
#define VIRTIO_WLAN_SCAN_TIMEOUT	(15 * HZ)
#define VIRTIO_WLAN_CONNECT_TIMEOUT	(30 * HZ)
#define VIRTIO_WLAN_EVENT_BUFS		128
#define VIRTIO_WLAN_EVENT_BUDGET	64
#define VIRTIO_WLAN_MAX_CONFIG_SIZE	16384
#define VIRTIO_WLAN_MAX_POWER		30
#define VIRTIO_WLAN_MAX_SCAN_SSIDS	16
#define VIRTIO_WLAN_MAX_SCAN_IE_LEN	2048

/* A received frame fits in one page together with both of its headers. */
#define VIRTIO_WLAN_MAX_MTU \
	(PAGE_SIZE - sizeof(struct virtio_net_hdr_v1) - ETH_HLEN)

static_assert(VIRTIO_WLAN_MAX_AKM_SUITES <= CFG80211_MAX_NUM_AKM_SUITES);
static_assert(NL80211_MAX_NR_CIPHER_SUITES <= VIRTIO_WLAN_MAX_PAIRWISE);
static_assert(IEEE80211_MAX_SSID_LEN == VIRTIO_WLAN_SSID_LEN);
static_assert(VIRTIO_WLAN_HE_PPE_LEN ==
	      sizeof_field(struct ieee80211_sta_he_cap, ppe_thres));

static const struct ieee80211_rate virtio_wlan_rates[] = {
	{ .bitrate = 10 },
	{ .bitrate = 20, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 55, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 110, .flags = IEEE80211_RATE_SHORT_PREAMBLE },
	{ .bitrate = 60 },
	{ .bitrate = 90 },
	{ .bitrate = 120 },
	{ .bitrate = 180 },
	{ .bitrate = 240 },
	{ .bitrate = 360 },
	{ .bitrate = 480 },
	{ .bitrate = 540 },
};

/*
 * Each band's channels lie in its frequency range. The 5 and 6 GHz bands
 * have only the OFDM rates.
 */
static const struct {
	enum nl80211_band band;
	u16 min_freq;
	u16 max_freq;
	unsigned int first_rate;
} virtio_wlan_bands[] = {
	[VIRTIO_WLAN_BAND_2GHZ] = { NL80211_BAND_2GHZ, 2412, 2484, 0 },
	[VIRTIO_WLAN_BAND_5GHZ] = { NL80211_BAND_5GHZ, 4910, 5925, 4 },
	[VIRTIO_WLAN_BAND_6GHZ] = { NL80211_BAND_6GHZ, 5935, 7115, 4 },
};

static const struct {
	u32 wire;
	u32 kernel;
} virtio_wlan_chan_flags[] = {
	{ VIRTIO_WLAN_CHAN_DISABLED, IEEE80211_CHAN_DISABLED },
	{ VIRTIO_WLAN_CHAN_NO_IR, IEEE80211_CHAN_NO_IR },
	{ VIRTIO_WLAN_CHAN_RADAR, IEEE80211_CHAN_RADAR },
	{ VIRTIO_WLAN_CHAN_NO_HT40PLUS, IEEE80211_CHAN_NO_HT40PLUS },
	{ VIRTIO_WLAN_CHAN_NO_HT40MINUS, IEEE80211_CHAN_NO_HT40MINUS },
	{ VIRTIO_WLAN_CHAN_NO_OFDM, IEEE80211_CHAN_NO_OFDM },
	{ VIRTIO_WLAN_CHAN_NO_80MHZ, IEEE80211_CHAN_NO_80MHZ },
	{ VIRTIO_WLAN_CHAN_NO_160MHZ, IEEE80211_CHAN_NO_160MHZ },
	{ VIRTIO_WLAN_CHAN_INDOOR_ONLY, IEEE80211_CHAN_INDOOR_ONLY },
	{ VIRTIO_WLAN_CHAN_IR_CONCURRENT, IEEE80211_CHAN_IR_CONCURRENT },
	{ VIRTIO_WLAN_CHAN_NO_20MHZ, IEEE80211_CHAN_NO_20MHZ },
	{ VIRTIO_WLAN_CHAN_NO_10MHZ, IEEE80211_CHAN_NO_10MHZ },
	{ VIRTIO_WLAN_CHAN_NO_HE, IEEE80211_CHAN_NO_HE },
};

static const u32 virtio_wlan_ciphers[] = {
	WLAN_CIPHER_SUITE_WEP40,
	WLAN_CIPHER_SUITE_WEP104,
	WLAN_CIPHER_SUITE_TKIP,
	WLAN_CIPHER_SUITE_CCMP,
	WLAN_CIPHER_SUITE_CCMP_256,
	WLAN_CIPHER_SUITE_GCMP,
	WLAN_CIPHER_SUITE_GCMP_256,
	WLAN_CIPHER_SUITE_AES_CMAC,
	WLAN_CIPHER_SUITE_BIP_CMAC_256,
	WLAN_CIPHER_SUITE_BIP_GMAC_128,
	WLAN_CIPHER_SUITE_BIP_GMAC_256,
};

struct virtio_wlan_band_data {
	struct ieee80211_supported_band sband;
	struct ieee80211_rate rates[ARRAY_SIZE(virtio_wlan_rates)];
	struct ieee80211_sband_iftype_data iftd;
};

/* The counters of misbehavior by the device, shown in debugfs. */
struct virtio_wlan_stats {
	u32 cmd_timeouts;
	u32 bad_responses;
	u32 bad_events;
	u32 stale_events;
	u32 bad_bss;
	u32 bad_connect_results;
	u32 bad_rates;
	u32 scan_timeouts;
	u32 connect_timeouts;
};

static const struct {
	const char *name;
	size_t offset;
} virtio_wlan_counters[] = {
	{ "cmd_timeouts", offsetof(struct virtio_wlan_stats, cmd_timeouts) },
	{ "bad_responses", offsetof(struct virtio_wlan_stats, bad_responses) },
	{ "bad_events", offsetof(struct virtio_wlan_stats, bad_events) },
	{ "stale_events", offsetof(struct virtio_wlan_stats, stale_events) },
	{ "bad_bss", offsetof(struct virtio_wlan_stats, bad_bss) },
	{ "bad_connect_results",
	  offsetof(struct virtio_wlan_stats, bad_connect_results) },
	{ "bad_rates", offsetof(struct virtio_wlan_stats, bad_rates) },
	{ "scan_timeouts", offsetof(struct virtio_wlan_stats, scan_timeouts) },
	{ "connect_timeouts",
	  offsetof(struct virtio_wlan_stats, connect_timeouts) },
};

enum virtio_wlan_conn_state {
	VIRTIO_WLAN_IDLE,
	VIRTIO_WLAN_CONNECTING,
	VIRTIO_WLAN_CONNECTED,
};

struct virtio_wlan {
	struct wiphy *wiphy;
	struct virtio_device *vdev;
	struct net_device *netdev;
	struct wireless_dev wdev;
	struct virtqueue *cmd_vq;
	struct virtqueue *event_vq;
	struct completion cmd_done;
	void *cmd_buf;
	size_t cmd_size;
	struct virtio_wlan_resp *resp_buf;
	size_t resp_size;
	void **event_bufs;
	unsigned int n_event_bufs;
	u8 mac[ETH_ALEN];
	u16 mtu;
	u32 max_event_size;
	struct virtio_wlan_band_data bands[ARRAY_SIZE(virtio_wlan_bands)];
	u32 ciphers[VIRTIO_WLAN_MAX_CIPHER_SUITES];
	/* The callbacks of the event queue do nothing until this is set. */
	bool ready;
	unsigned long event_scheduled;
	struct wiphy_delayed_work event_work;
	struct wiphy_delayed_work scan_timeout;
	struct wiphy_delayed_work conn_timeout;
	/* These are changed under the wiphy lock. */
	bool removing;
	bool broken;
	u32 last_id;
	struct cfg80211_scan_request *scan_req;
	u32 scan_id;
	enum virtio_wlan_conn_state conn_state;
	u32 conn_id;
	u8 conn_ssid[IEEE80211_MAX_SSID_LEN];
	u8 conn_ssid_len;
	struct virtio_wlan_stats stats;
};

static void virtio_wlan_cmd_done(struct virtqueue *vq)
{
	struct virtio_wlan *vw = vq->vdev->priv;

	complete(&vw->cmd_done);
}

static void virtio_wlan_finish_scan(struct virtio_wlan *vw, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };

	if (!vw->scan_req)
		return;
	wiphy_delayed_work_cancel(vw->wiphy, &vw->scan_timeout);
	cfg80211_scan_done(vw->scan_req, &info);
	vw->scan_req = NULL;
}

/* Stop tracking the connection or attempt, without reporting anything. */
static enum virtio_wlan_conn_state virtio_wlan_end_conn(struct virtio_wlan *vw)
{
	enum virtio_wlan_conn_state state = vw->conn_state;

	vw->conn_state = VIRTIO_WLAN_IDLE;
	wiphy_delayed_work_cancel(vw->wiphy, &vw->conn_timeout);
	return state;
}

/*
 * The device keeps the buffers of a command that it does not answer, so it
 * is not used again. A pending scan, connection or attempt ends at once.
 */
static void virtio_wlan_break(struct virtio_wlan *vw)
{
	vw->broken = true;
	virtio_wlan_finish_scan(vw, true);
	switch (virtio_wlan_end_conn(vw)) {
	case VIRTIO_WLAN_CONNECTED:
		cfg80211_disconnected(vw->netdev, WLAN_REASON_UNSPECIFIED, NULL,
				      0, true, GFP_KERNEL);
		break;
	case VIRTIO_WLAN_CONNECTING:
		cfg80211_connect_timeout(vw->netdev, NULL, NULL, 0, GFP_KERNEL,
					 NL80211_TIMEOUT_UNSPECIFIED);
		break;
	case VIRTIO_WLAN_IDLE:
		break;
	}
}

/*
 * Send a command and wait for its response, whose payload of up to
 * resp_size bytes is copied to resp. A command without a response within
 * the deadline leaves its buffers with the device, which is then broken.
 */
static int virtio_wlan_cmd(struct virtio_wlan *vw, u16 type, const void *req,
			   size_t req_len, void *resp, size_t resp_size,
			   size_t *resp_len)
{
	struct virtio_wlan_cmd_hdr *hdr = vw->cmd_buf;
	struct virtio_wlan_resp *r = vw->resp_buf;
	struct scatterlist out, in, *sgs[] = { &out, &in };
	unsigned long deadline;
	unsigned int len;
	long left;
	size_t n;
	int err;

	lockdep_assert_wiphy(vw->wiphy);

	if (vw->removing || vw->broken)
		return -ENODEV;
	if (WARN_ON(req_len > vw->cmd_size - sizeof(*hdr) ||
		    resp_size > vw->resp_size))
		return -EINVAL;

	hdr->type = cpu_to_le16(type);
	hdr->flags = 0;
	hdr->len = cpu_to_le32(req_len);
	if (req_len)
		memcpy(hdr + 1, req, req_len);
	sg_init_one(&out, hdr, sizeof(*hdr) + req_len);
	sg_init_one(&in, r, sizeof(*r) + resp_size);

	reinit_completion(&vw->cmd_done);
	err = virtqueue_add_sgs(vw->cmd_vq, sgs, 1, 1, vw, GFP_KERNEL);
	if (err) {
		memzero_explicit(hdr + 1, req_len);
		return err;
	}

	deadline = jiffies + VIRTIO_WLAN_CMD_TIMEOUT;
	virtqueue_kick(vw->cmd_vq);
	for (;;) {
		if (virtqueue_get_buf(vw->cmd_vq, &len))
			break;
		left = (long)(deadline - jiffies);
		if (virtqueue_is_broken(vw->cmd_vq) || left <= 0) {
			memzero_explicit(hdr + 1, req_len);
			vw->stats.cmd_timeouts++;
			dev_err(&vw->vdev->dev, "command %u got no response\n",
				type);
			virtio_wlan_break(vw);
			return -ETIMEDOUT;
		}
		wait_for_completion_timeout(&vw->cmd_done, left);
	}
	/* Requests can hold keys. */
	memzero_explicit(hdr + 1, req_len);

	/* The used length is not trusted, so it is checked against the buffer. */
	if (len < sizeof(*r) || len > sizeof(*r) + resp_size ||
	    le32_to_cpu(r->len) > len - sizeof(*r)) {
		vw->stats.bad_responses++;
		return -EIO;
	}
	err = virtio_wlan_status_to_errno(le16_to_cpu(r->status));
	if (err)
		return err;

	n = le32_to_cpu(r->len);
	if (n)
		memcpy(resp, r + 1, n);
	if (resp_len)
		*resp_len = n;
	return 0;
}

static int virtio_wlan_add_channels(struct ieee80211_supported_band *sband,
				    u8 band,
				    const struct virtio_wlan_channel *in,
				    unsigned int n)
{
	struct ieee80211_channel *chans;
	unsigned int i, j;
	u16 prev = 0;

	chans = kcalloc(n, sizeof(*chans), GFP_KERNEL);
	if (!chans)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		u16 freq = le16_to_cpu(in[i].freq);
		u32 flags = le32_to_cpu(in[i].flags);
		struct cfg80211_chan_def def;

		chans[i].band = sband->band;
		chans[i].center_freq = freq;

		/*
		 * Ascending frequencies also exclude duplicate channels.
		 * cfg80211 warns when it checks a connection on a channel
		 * whose 20 MHz definition is invalid, such as a 6 GHz
		 * frequency off the 20 MHz raster.
		 */
		cfg80211_chandef_create(&def, &chans[i], NL80211_CHAN_NO_HT);
		if (freq <= prev || freq < virtio_wlan_bands[band].min_freq ||
		    freq > virtio_wlan_bands[band].max_freq ||
		    !cfg80211_chandef_valid(&def)) {
			kfree(chans);
			return -EINVAL;
		}
		prev = freq;

		chans[i].max_power = min_t(u16, le16_to_cpu(in[i].max_power),
					   VIRTIO_WLAN_MAX_POWER);
		for (j = 0; j < ARRAY_SIZE(virtio_wlan_chan_flags); j++)
			if (flags & virtio_wlan_chan_flags[j].wire)
				chans[i].flags |= virtio_wlan_chan_flags[j].kernel;
	}

	sband->channels = chans;
	sband->n_channels = n;
	return 0;
}

static void virtio_wlan_band_caps(struct virtio_wlan_band_data *data,
				  const struct virtio_wlan_band *in)
{
	struct ieee80211_supported_band *sband = &data->sband;
	struct ieee80211_sta_he_cap *he = &data->iftd.he_cap;

	/* cfg80211 refuses HT and VHT on the 6 GHz band. */
	if ((in->flags & VIRTIO_WLAN_BAND_F_HT) &&
	    sband->band != NL80211_BAND_6GHZ) {
		sband->ht_cap.ht_supported = true;
		sband->ht_cap.cap = le16_to_cpu(in->ht.cap);
		sband->ht_cap.ampdu_factor = in->ht.ampdu_factor &
			IEEE80211_HT_AMPDU_PARM_FACTOR;
		sband->ht_cap.ampdu_density = in->ht.ampdu_density &
			(IEEE80211_HT_AMPDU_PARM_DENSITY >>
			 IEEE80211_HT_AMPDU_PARM_DENSITY_SHIFT);
		memcpy(sband->ht_cap.mcs.rx_mask, in->ht.rx_mask,
		       sizeof(sband->ht_cap.mcs.rx_mask));
		sband->ht_cap.mcs.rx_highest = in->ht.rx_highest;
		sband->ht_cap.mcs.tx_params = in->ht.tx_params;
	}

	if ((in->flags & VIRTIO_WLAN_BAND_F_VHT) &&
	    sband->band != NL80211_BAND_6GHZ) {
		sband->vht_cap.vht_supported = true;
		sband->vht_cap.cap = le32_to_cpu(in->vht.cap);
		sband->vht_cap.vht_mcs.rx_mcs_map = in->vht.rx_mcs_map;
		sband->vht_cap.vht_mcs.rx_highest = in->vht.rx_highest;
		sband->vht_cap.vht_mcs.tx_mcs_map = in->vht.tx_mcs_map;
		sband->vht_cap.vht_mcs.tx_highest = in->vht.tx_highest;
	}

	if (!(in->flags & VIRTIO_WLAN_BAND_F_HE))
		return;

	data->iftd.types_mask = BIT(NL80211_IFTYPE_STATION);
	he->has_he = true;
	memcpy(he->he_cap_elem.mac_cap_info, in->he.mac_cap,
	       sizeof(he->he_cap_elem.mac_cap_info));
	memcpy(he->he_cap_elem.phy_cap_info, in->he.phy_cap,
	       sizeof(he->he_cap_elem.phy_cap_info));
	he->he_mcs_nss_supp.rx_mcs_80 = in->he.rx_mcs_80;
	he->he_mcs_nss_supp.tx_mcs_80 = in->he.tx_mcs_80;
	he->he_mcs_nss_supp.rx_mcs_160 = in->he.rx_mcs_160;
	he->he_mcs_nss_supp.tx_mcs_160 = in->he.tx_mcs_160;
	he->he_mcs_nss_supp.rx_mcs_80p80 = in->he.rx_mcs_80p80;
	he->he_mcs_nss_supp.tx_mcs_80p80 = in->he.tx_mcs_80p80;
	memcpy(he->ppe_thres, in->he.ppe_thres, sizeof(he->ppe_thres));
	if ((in->flags & VIRTIO_WLAN_BAND_F_HE_6GHZ) &&
	    sband->band == NL80211_BAND_6GHZ)
		data->iftd.he_6ghz_capa.capa = in->he_6ghz_capa;
	_ieee80211_set_sband_iftype_data(sband, &data->iftd, 1);
}

/*
 * Build a band from its description, which is followed by its channels. A
 * band that wiphy_register() would refuse is left out with a warning.
 */
static int virtio_wlan_add_band(struct virtio_wlan *vw,
				const struct virtio_wlan_band *in)
{
	struct virtio_wlan_band_data *data = &vw->bands[in->band];
	struct ieee80211_supported_band *sband = &data->sband;
	unsigned int first = virtio_wlan_bands[in->band].first_rate;
	unsigned int n = le16_to_cpu(in->n_channels);
	int err;

	if (!n) {
		dev_warn(&vw->vdev->dev, "band %u has no channels\n", in->band);
		return 0;
	}
	if (in->band == VIRTIO_WLAN_BAND_6GHZ &&
	    !(in->flags & VIRTIO_WLAN_BAND_F_HE)) {
		dev_warn(&vw->vdev->dev, "the 6 GHz band has no HE capabilities\n");
		return 0;
	}

	sband->band = virtio_wlan_bands[in->band].band;
	err = virtio_wlan_add_channels(sband, in->band, (const void *)(in + 1),
				       n);
	if (err)
		return err;

	memcpy(data->rates, &virtio_wlan_rates[first],
	       sizeof(virtio_wlan_rates) - first * sizeof(virtio_wlan_rates[0]));
	sband->bitrates = data->rates;
	sband->n_bitrates = ARRAY_SIZE(virtio_wlan_rates) - first;
	virtio_wlan_band_caps(data, in);
	vw->wiphy->bands[sband->band] = sband;
	return 0;
}

static void virtio_wlan_add_ciphers(struct virtio_wlan *vw,
				    const struct virtio_wlan_wiphy *w)
{
	struct wiphy *wiphy = vw->wiphy;
	int i, j, k;

	for (i = 0; i < w->n_cipher_suites; i++) {
		u32 cipher = le32_to_cpu(w->cipher_suites[i]);

		for (j = 0; j < ARRAY_SIZE(virtio_wlan_ciphers); j++)
			if (virtio_wlan_ciphers[j] == cipher)
				break;
		for (k = 0; k < wiphy->n_cipher_suites; k++)
			if (vw->ciphers[k] == cipher)
				break;
		if (j < ARRAY_SIZE(virtio_wlan_ciphers) &&
		    k == wiphy->n_cipher_suites)
			vw->ciphers[wiphy->n_cipher_suites++] = cipher;
	}
	wiphy->cipher_suites = vw->ciphers;
}

static int virtio_wlan_parse_wiphy(struct virtio_wlan *vw, const u8 *p,
				   size_t len)
{
	const struct virtio_wlan_wiphy *w = (const void *)p;
	struct wiphy *wiphy = vw->wiphy;
	size_t off = sizeof(*w);
	unsigned long seen = 0;
	u32 flags;
	int i, err;

	if (len < sizeof(*w) || w->n_bands > ARRAY_SIZE(virtio_wlan_bands) ||
	    w->n_cipher_suites > VIRTIO_WLAN_MAX_CIPHER_SUITES ||
	    !w->max_scan_ssids)
		return -EINVAL;

	for (i = 0; i < w->n_bands; i++) {
		const struct virtio_wlan_band *b = (const void *)(p + off);
		size_t n;

		if (len - off < sizeof(*b))
			return -EINVAL;
		n = le16_to_cpu(b->n_channels);
		if ((len - off - sizeof(*b)) / sizeof(struct virtio_wlan_channel) < n ||
		    b->band >= ARRAY_SIZE(virtio_wlan_bands) ||
		    __test_and_set_bit(b->band, &seen))
			return -EINVAL;
		err = virtio_wlan_add_band(vw, b);
		if (err)
			return err;
		off += sizeof(*b) + n * sizeof(struct virtio_wlan_channel);
	}
	if (off != len)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(vw->bands); i++)
		if (vw->bands[i].sband.n_channels)
			break;
	if (i == ARRAY_SIZE(vw->bands)) {
		dev_err(&vw->vdev->dev, "the device has no usable band\n");
		return -EINVAL;
	}

	virtio_wlan_add_ciphers(vw, w);
	wiphy->max_scan_ssids = min(w->max_scan_ssids,
				    VIRTIO_WLAN_MAX_SCAN_SSIDS);
	wiphy->max_scan_ie_len = min(le16_to_cpu(w->max_scan_ie_len),
				     VIRTIO_WLAN_MAX_SCAN_IE_LEN);
	wiphy->max_num_akm_suites = clamp(w->max_akm_suites,
					  NL80211_MAX_NR_AKM_SUITES,
					  VIRTIO_WLAN_MAX_AKM_SUITES);

	flags = le32_to_cpu(w->flags);
	if (flags & VIRTIO_WLAN_WIPHY_F_SCAN_RANDOM_MAC)
		wiphy->features |= NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR;
	if (flags & VIRTIO_WLAN_WIPHY_F_MFP_OPTIONAL)
		wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_MFP_OPTIONAL);
	if (flags & VIRTIO_WLAN_WIPHY_F_CAN_REPLACE_PTK0)
		wiphy_ext_feature_set(wiphy,
				      NL80211_EXT_FEATURE_CAN_REPLACE_PTK0);
	return 0;
}

/*
 * The largest command is a scan of every channel with the most SSIDs and IEs,
 * or a connection with the most IEs that nl80211 accepts.
 */
static size_t virtio_wlan_max_cmd_size(struct wiphy *wiphy)
{
	unsigned int n_channels = 0;
	size_t scan;
	int i;

	for (i = 0; i < NUM_NL80211_BANDS; i++)
		if (wiphy->bands[i])
			n_channels += wiphy->bands[i]->n_channels;
	scan = sizeof(struct virtio_wlan_scan) +
	       wiphy->max_scan_ssids * sizeof(struct virtio_wlan_ssid) +
	       n_channels * sizeof(__le16) + wiphy->max_scan_ie_len;
	return sizeof(struct virtio_wlan_cmd_hdr) +
	       max(scan, sizeof(struct virtio_wlan_connect) +
			 IEEE80211_MAX_DATA_LEN);
}

static int virtio_wlan_get_wiphy(struct virtio_wlan *vw)
{
	size_t len;
	void *buf;
	int err;

	buf = kmalloc(vw->resp_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	err = virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_GET_WIPHY, NULL, 0, buf,
			      vw->resp_size, &len);
	if (err) {
		dev_err(&vw->vdev->dev, "GET_WIPHY failed: %d\n", err);
	} else {
		err = virtio_wlan_parse_wiphy(vw, buf, len);
		if (err == -EINVAL)
			dev_err(&vw->vdev->dev,
				"the wiphy description is invalid\n");
	}
	kfree(buf);
	if (err)
		return err;

	len = virtio_wlan_max_cmd_size(vw->wiphy);
	buf = krealloc(vw->cmd_buf, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	vw->cmd_buf = buf;
	vw->cmd_size = len;
	return 0;
}

static u32 virtio_wlan_new_id(struct virtio_wlan *vw)
{
	return ++vw->last_id;
}

static void virtio_wlan_send_abort_scan(struct virtio_wlan *vw, u32 id)
{
	struct virtio_wlan_abort_scan a = { .id = cpu_to_le32(id) };

	virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_ABORT_SCAN, &a, sizeof(a), NULL, 0,
			NULL);
}

static void virtio_wlan_send_disconnect(struct virtio_wlan *vw, u32 id,
					u16 reason)
{
	struct virtio_wlan_disconnect d = {
		.id = cpu_to_le32(id),
		.reason = cpu_to_le16(reason),
	};

	virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_DISCONNECT, &d, sizeof(d), NULL, 0,
			NULL);
}

/* End a connection or attempt in the device without reporting it. */
static void virtio_wlan_drop_conn(struct virtio_wlan *vw)
{
	u32 id = vw->conn_id;

	if (virtio_wlan_end_conn(vw) != VIRTIO_WLAN_IDLE)
		virtio_wlan_send_disconnect(vw, id, WLAN_REASON_UNSPECIFIED);
}

static int virtio_wlan_scan(struct wiphy *wiphy,
			    struct cfg80211_scan_request *req)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_ssid *ssids;
	struct virtio_wlan_scan *scan;
	__le16 *freqs;
	size_t len;
	int i, err;
	u32 id;

	if (vw->scan_req)
		return -EBUSY;

	len = sizeof(*scan) + req->n_ssids * sizeof(*ssids) +
	      req->n_channels * sizeof(*freqs) + req->ie_len;
	scan = kzalloc(len, GFP_KERNEL);
	if (!scan)
		return -ENOMEM;

	id = virtio_wlan_new_id(vw);
	scan->id = cpu_to_le32(id);
	if (req->flags & NL80211_SCAN_FLAG_RANDOM_ADDR) {
		scan->flags = cpu_to_le32(VIRTIO_WLAN_SCAN_F_RANDOM_MAC);
		ether_addr_copy(scan->mac_addr, req->mac_addr);
		ether_addr_copy(scan->mac_addr_mask, req->mac_addr_mask);
	}
	scan->n_ssids = cpu_to_le16(req->n_ssids);
	scan->n_freqs = cpu_to_le16(req->n_channels);
	scan->ie_len = cpu_to_le16(req->ie_len);
	ether_addr_copy(scan->bssid, req->bssid);

	ssids = (void *)(scan + 1);
	for (i = 0; i < req->n_ssids; i++) {
		ssids[i].len = req->ssids[i].ssid_len;
		memcpy(ssids[i].ssid, req->ssids[i].ssid,
		       req->ssids[i].ssid_len);
	}
	freqs = (void *)(ssids + req->n_ssids);
	for (i = 0; i < req->n_channels; i++)
		freqs[i] = cpu_to_le16(req->channels[i]->center_freq);
	if (req->ie_len)
		memcpy(freqs + req->n_channels, req->ie, req->ie_len);

	err = virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_SCAN, scan, len, NULL, 0,
			      NULL);
	kfree(scan);
	if (err)
		return err;

	vw->scan_req = req;
	vw->scan_id = id;
	wiphy_delayed_work_queue(wiphy, &vw->scan_timeout,
				 VIRTIO_WLAN_SCAN_TIMEOUT);
	return 0;
}

static void virtio_wlan_abort_scan(struct wiphy *wiphy,
				   struct wireless_dev *wdev)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);

	if (vw->scan_req)
		virtio_wlan_send_abort_scan(vw, vw->scan_id);
}

static void virtio_wlan_scan_timeout(struct wiphy *wiphy,
				     struct wiphy_work *work)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);

	if (vw->removing || vw->broken || !vw->scan_req)
		return;
	vw->stats.scan_timeouts++;
	virtio_wlan_send_abort_scan(vw, vw->scan_id);
	virtio_wlan_finish_scan(vw, true);
}

static int virtio_wlan_auth_type(enum nl80211_auth_type type)
{
	switch (type) {
	case NL80211_AUTHTYPE_OPEN_SYSTEM:
		return VIRTIO_WLAN_AUTH_OPEN;
	case NL80211_AUTHTYPE_SHARED_KEY:
		return VIRTIO_WLAN_AUTH_SHARED_KEY;
	case NL80211_AUTHTYPE_AUTOMATIC:
		return VIRTIO_WLAN_AUTH_AUTOMATIC;
	default:
		return -EOPNOTSUPP;
	}
}

static int virtio_wlan_mfp(enum nl80211_mfp mfp)
{
	switch (mfp) {
	case NL80211_MFP_NO:
		return VIRTIO_WLAN_MFP_NO;
	case NL80211_MFP_REQUIRED:
		return VIRTIO_WLAN_MFP_REQUIRED;
	case NL80211_MFP_OPTIONAL:
		return VIRTIO_WLAN_MFP_OPTIONAL;
	default:
		return -EOPNOTSUPP;
	}
}

static const struct {
	u32 kernel;
	u32 wire;
} virtio_wlan_connect_flags[] = {
	{ ASSOC_REQ_DISABLE_HT, VIRTIO_WLAN_CONNECT_F_DISABLE_HT },
	{ ASSOC_REQ_DISABLE_VHT, VIRTIO_WLAN_CONNECT_F_DISABLE_VHT },
	{ ASSOC_REQ_DISABLE_HE, VIRTIO_WLAN_CONNECT_F_DISABLE_HE },
	{ ASSOC_REQ_DISABLE_EHT, VIRTIO_WLAN_CONNECT_F_DISABLE_EHT },
};

static void virtio_wlan_fill_connect(struct virtio_wlan_connect *c,
				     const struct cfg80211_connect_params *sme)
{
	const struct cfg80211_crypto_settings *crypto = &sme->crypto;
	u32 flags = 0;
	int i;

	if (sme->privacy)
		flags |= VIRTIO_WLAN_CONNECT_F_PRIVACY;
	if (crypto->control_port)
		flags |= VIRTIO_WLAN_CONNECT_F_CONTROL_PORT;
	if (crypto->control_port_no_encrypt)
		flags |= VIRTIO_WLAN_CONNECT_F_CONTROL_PORT_NO_ENCRYPT;
	for (i = 0; i < ARRAY_SIZE(virtio_wlan_connect_flags); i++)
		if (sme->flags & virtio_wlan_connect_flags[i].kernel)
			flags |= virtio_wlan_connect_flags[i].wire;
	c->flags = cpu_to_le32(flags);

	c->wpa_versions = cpu_to_le32(crypto->wpa_versions &
				      (VIRTIO_WLAN_WPA_VERSION_1 |
				       VIRTIO_WLAN_WPA_VERSION_2 |
				       VIRTIO_WLAN_WPA_VERSION_3));
	c->cipher_group = cpu_to_le32(crypto->cipher_group);
	c->n_ciphers_pairwise = crypto->n_ciphers_pairwise;
	for (i = 0; i < crypto->n_ciphers_pairwise; i++)
		c->ciphers_pairwise[i] = cpu_to_le32(crypto->ciphers_pairwise[i]);
	c->n_akm_suites = crypto->n_akm_suites;
	for (i = 0; i < crypto->n_akm_suites; i++)
		c->akm_suites[i] = cpu_to_le32(crypto->akm_suites[i]);
	c->control_port_ethertype =
		cpu_to_le16(be16_to_cpu(crypto->control_port_ethertype));

	if (sme->bssid)
		ether_addr_copy(c->bssid, sme->bssid);
	if (sme->bssid_hint)
		ether_addr_copy(c->bssid_hint, sme->bssid_hint);
	if (sme->channel)
		c->freq = cpu_to_le16(sme->channel->center_freq);
	if (sme->channel_hint)
		c->freq_hint = cpu_to_le16(sme->channel_hint->center_freq);
	c->ssid_len = sme->ssid_len;
	memcpy(c->ssid, sme->ssid, sme->ssid_len);
	if (sme->key && sme->key_len) {
		c->key_idx = sme->key_idx;
		c->key_len = sme->key_len;
		memcpy(c->key, sme->key, sme->key_len);
	}
	c->ie_len = cpu_to_le16(sme->ie_len);
	if (sme->ie_len)
		memcpy(c + 1, sme->ie, sme->ie_len);
}

static int virtio_wlan_connect(struct wiphy *wiphy, struct net_device *dev,
			       struct cfg80211_connect_params *sme)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct wireless_dev *wdev = dev->ieee80211_ptr;
	struct virtio_wlan_connect *c;
	int auth, mfp, err;
	unsigned long flags;
	bool queued;
	size_t len;
	u32 id;

	/* The device keeps one connection, which a roam cannot replace. */
	if (wdev->connected)
		return -EALREADY;

	/*
	 * An outcome that cfg80211 has not processed yet completes or fails
	 * this request, so the device is left alone.
	 */
	spin_lock_irqsave(&wdev->event_lock, flags);
	queued = !list_empty(&wdev->event_list);
	spin_unlock_irqrestore(&wdev->event_lock, flags);
	if (queued)
		return 0;

	/*
	 * cfg80211 also calls connect during an attempt for the same SSID,
	 * and forgets the attempt if this fails, so the attempt ends before
	 * anything can fail.
	 */
	virtio_wlan_drop_conn(vw);

	auth = virtio_wlan_auth_type(sme->auth_type);
	mfp = virtio_wlan_mfp(sme->mfp);
	if (auth < 0 || mfp < 0)
		return -EOPNOTSUPP;
	if (sme->ie_len > IEEE80211_MAX_DATA_LEN ||
	    sme->ssid_len > IEEE80211_MAX_SSID_LEN ||
	    sme->key_len > VIRTIO_WLAN_WEP_KEY_LEN ||
	    sme->crypto.n_ciphers_pairwise > VIRTIO_WLAN_MAX_PAIRWISE ||
	    sme->crypto.n_akm_suites > VIRTIO_WLAN_MAX_AKM_SUITES)
		return -EINVAL;

	len = sizeof(*c) + sme->ie_len;
	c = kzalloc(len, GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	id = virtio_wlan_new_id(vw);
	c->id = cpu_to_le32(id);
	c->auth_type = auth;
	c->mfp = mfp;
	virtio_wlan_fill_connect(c, sme);

	err = virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_CONNECT, c, len, NULL, 0,
			      NULL);
	kfree_sensitive(c);
	if (err)
		return err;

	vw->conn_state = VIRTIO_WLAN_CONNECTING;
	vw->conn_id = id;
	memcpy(vw->conn_ssid, sme->ssid, sme->ssid_len);
	vw->conn_ssid_len = sme->ssid_len;
	wiphy_delayed_work_queue(wiphy, &vw->conn_timeout,
				 VIRTIO_WLAN_CONNECT_TIMEOUT);
	return 0;
}

/*
 * The device's own report of the disconnection carries an id that has
 * ended, so an established connection is reported here, and an attempt
 * ends without a report, since cfg80211 has already forgotten it.
 */
static int virtio_wlan_disconnect(struct wiphy *wiphy, struct net_device *dev,
				  u16 reason)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	enum virtio_wlan_conn_state state;
	u32 id = vw->conn_id;

	state = virtio_wlan_end_conn(vw);
	if (state != VIRTIO_WLAN_IDLE)
		virtio_wlan_send_disconnect(vw, id, reason);
	if (state == VIRTIO_WLAN_CONNECTED)
		cfg80211_disconnected(dev, reason, NULL, 0, true, GFP_KERNEL);
	return 0;
}

static void virtio_wlan_conn_timeout(struct wiphy *wiphy,
				     struct wiphy_work *work)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);

	if (vw->removing || vw->broken ||
	    vw->conn_state != VIRTIO_WLAN_CONNECTING)
		return;
	vw->stats.connect_timeouts++;
	virtio_wlan_drop_conn(vw);
	cfg80211_connect_timeout(vw->netdev, NULL, NULL, 0, GFP_KERNEL,
				 NL80211_TIMEOUT_ASSOC);
}

static int virtio_wlan_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			       int link_id, u8 idx, bool pairwise,
			       const u8 *mac, struct key_params *params)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_add_key k = {};
	int err;

	if (link_id >= 0 || params->mode != NL80211_KEY_RX_TX)
		return -EOPNOTSUPP;
	if (params->key_len > VIRTIO_WLAN_KEY_LEN ||
	    params->seq_len > VIRTIO_WLAN_SEQ_LEN)
		return -EINVAL;

	k.cipher = cpu_to_le32(params->cipher);
	k.index = idx;
	k.flags = pairwise ? VIRTIO_WLAN_KEY_F_PAIRWISE : 0;
	k.key_len = params->key_len;
	k.seq_len = params->seq_len;
	if (mac)
		ether_addr_copy(k.peer, mac);
	memcpy(k.key, params->key, params->key_len);
	if (params->seq_len)
		memcpy(k.seq, params->seq, params->seq_len);

	err = virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_ADD_KEY, &k, sizeof(k), NULL,
			      0, NULL);
	memzero_explicit(&k, sizeof(k));
	return err;
}

static int virtio_wlan_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			       int link_id, u8 idx, bool pairwise,
			       const u8 *mac)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_del_key k = {
		.index = idx,
		.flags = pairwise ? VIRTIO_WLAN_KEY_F_PAIRWISE : 0,
	};

	if (link_id >= 0)
		return -EOPNOTSUPP;
	if (mac)
		ether_addr_copy(k.peer, mac);
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_DEL_KEY, &k, sizeof(k), NULL,
			       0, NULL);
}

static int virtio_wlan_set_default_key(struct wiphy *wiphy,
				       struct net_device *dev, int link_id,
				       u8 idx, bool unicast, bool multicast)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_set_default_key k = { .index = idx };

	if (link_id >= 0)
		return -EOPNOTSUPP;
	if (unicast)
		k.flags |= VIRTIO_WLAN_DEFAULT_KEY_F_UNICAST;
	if (multicast)
		k.flags |= VIRTIO_WLAN_DEFAULT_KEY_F_MULTICAST;
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_SET_DEFAULT_KEY, &k,
			       sizeof(k), NULL, 0, NULL);
}

static int virtio_wlan_set_default_mgmt_key(struct wiphy *wiphy,
					    struct wireless_dev *wdev,
					    int link_id, u8 idx)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_set_default_mgmt_key k = { .index = idx };

	if (link_id >= 0)
		return -EOPNOTSUPP;
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY, &k,
			       sizeof(k), NULL, 0, NULL);
}

static int virtio_wlan_change_station(struct wiphy *wiphy,
				      struct wireless_dev *wdev, const u8 *mac,
				      struct station_parameters *params)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_authorize a = {};

	if (params->sta_flags_mask & ~BIT(NL80211_STA_FLAG_AUTHORIZED))
		return -EOPNOTSUPP;
	if (!params->sta_flags_mask)
		return 0;

	ether_addr_copy(a.peer, mac);
	a.authorized = !!(params->sta_flags_set &
			  BIT(NL80211_STA_FLAG_AUTHORIZED));
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_AUTHORIZE, &a, sizeof(a),
			       NULL, 0, NULL);
}

static const enum rate_info_bw virtio_wlan_rate_bws[] = {
	[VIRTIO_WLAN_RATE_BW_20] = RATE_INFO_BW_20,
	[VIRTIO_WLAN_RATE_BW_5] = RATE_INFO_BW_5,
	[VIRTIO_WLAN_RATE_BW_10] = RATE_INFO_BW_10,
	[VIRTIO_WLAN_RATE_BW_40] = RATE_INFO_BW_40,
	[VIRTIO_WLAN_RATE_BW_80] = RATE_INFO_BW_80,
	[VIRTIO_WLAN_RATE_BW_160] = RATE_INFO_BW_160,
	[VIRTIO_WLAN_RATE_BW_HE_RU] = RATE_INFO_BW_HE_RU,
};

/*
 * Build a rate, or return false for one outside the ranges that
 * cfg80211_calculate_bitrate() and nl80211 accept for its type, on which
 * they warn.
 */
static bool virtio_wlan_rate(const struct virtio_wlan_rate *in,
			     struct rate_info *out)
{
	bool short_gi = in->flags & VIRTIO_WLAN_RATE_F_SHORT_GI;
	enum rate_info_bw bw;

	if (in->bw >= ARRAY_SIZE(virtio_wlan_rate_bws))
		return false;
	bw = virtio_wlan_rate_bws[in->bw];
	memset(out, 0, sizeof(*out));
	out->bw = bw;

	switch (in->type) {
	case VIRTIO_WLAN_RATE_LEGACY:
		if (bw != RATE_INFO_BW_20 && bw != RATE_INFO_BW_5 &&
		    bw != RATE_INFO_BW_10)
			return false;
		out->legacy = le16_to_cpu(in->legacy);
		return true;
	case VIRTIO_WLAN_RATE_HT:
		if (in->mcs >= 32 || (bw != RATE_INFO_BW_20 &&
				      bw != RATE_INFO_BW_40))
			return false;
		out->flags = RATE_INFO_FLAGS_MCS;
		break;
	case VIRTIO_WLAN_RATE_VHT:
		if (in->mcs > 11 || in->nss < 1 || in->nss > 8 ||
		    bw == RATE_INFO_BW_5 || bw == RATE_INFO_BW_10 ||
		    bw == RATE_INFO_BW_HE_RU)
			return false;
		out->flags = RATE_INFO_FLAGS_VHT_MCS;
		out->nss = in->nss;
		break;
	case VIRTIO_WLAN_RATE_HE:
		if (in->mcs > 11 || in->nss < 1 || in->nss > 8 ||
		    in->he_gi > NL80211_RATE_INFO_HE_GI_3_2 || in->he_dcm > 1 ||
		    in->he_ru_alloc > NL80211_RATE_INFO_HE_RU_ALLOC_2x996 ||
		    bw == RATE_INFO_BW_5 || bw == RATE_INFO_BW_10)
			return false;
		out->flags = RATE_INFO_FLAGS_HE_MCS;
		out->nss = in->nss;
		out->he_gi = in->he_gi;
		out->he_dcm = in->he_dcm;
		out->he_ru_alloc = in->he_ru_alloc;
		short_gi = false;
		break;
	default:
		return false;
	}
	out->mcs = in->mcs;
	if (short_gi)
		out->flags |= RATE_INFO_FLAGS_SHORT_GI;
	return true;
}

static const struct {
	u32 wire;
	enum nl80211_sta_info kernel;
} virtio_wlan_sta_fields[] = {
	{ VIRTIO_WLAN_STA_CONNECTED_TIME, NL80211_STA_INFO_CONNECTED_TIME },
	{ VIRTIO_WLAN_STA_INACTIVE_TIME, NL80211_STA_INFO_INACTIVE_TIME },
	{ VIRTIO_WLAN_STA_RX_BYTES, NL80211_STA_INFO_RX_BYTES64 },
	{ VIRTIO_WLAN_STA_TX_BYTES, NL80211_STA_INFO_TX_BYTES64 },
	{ VIRTIO_WLAN_STA_RX_PACKETS, NL80211_STA_INFO_RX_PACKETS },
	{ VIRTIO_WLAN_STA_TX_PACKETS, NL80211_STA_INFO_TX_PACKETS },
	{ VIRTIO_WLAN_STA_TX_RETRIES, NL80211_STA_INFO_TX_RETRIES },
	{ VIRTIO_WLAN_STA_TX_FAILED, NL80211_STA_INFO_TX_FAILED },
	{ VIRTIO_WLAN_STA_SIGNAL, NL80211_STA_INFO_SIGNAL },
	{ VIRTIO_WLAN_STA_SIGNAL_AVG, NL80211_STA_INFO_SIGNAL_AVG },
};

/* A rate that fails its checks is left out of the station info. */
static void virtio_wlan_station_info(struct virtio_wlan *vw,
				     const struct virtio_wlan_station_info *in,
				     struct station_info *sinfo)
{
	u32 filled = le32_to_cpu(in->filled);
	int i;

	sinfo->connected_time = le32_to_cpu(in->connected_time);
	sinfo->inactive_time = le32_to_cpu(in->inactive_time);
	sinfo->rx_bytes = le64_to_cpu(in->rx_bytes);
	sinfo->tx_bytes = le64_to_cpu(in->tx_bytes);
	sinfo->rx_packets = le32_to_cpu(in->rx_packets);
	sinfo->tx_packets = le32_to_cpu(in->tx_packets);
	sinfo->tx_retries = le32_to_cpu(in->tx_retries);
	sinfo->tx_failed = le32_to_cpu(in->tx_failed);
	sinfo->signal = in->signal;
	sinfo->signal_avg = in->signal_avg;
	for (i = 0; i < ARRAY_SIZE(virtio_wlan_sta_fields); i++)
		if (filled & virtio_wlan_sta_fields[i].wire)
			sinfo->filled |= BIT_ULL(virtio_wlan_sta_fields[i].kernel);

	if (filled & VIRTIO_WLAN_STA_TX_RATE) {
		if (virtio_wlan_rate(&in->tx_rate, &sinfo->txrate))
			sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		else
			vw->stats.bad_rates++;
	}
	if (filled & VIRTIO_WLAN_STA_RX_RATE) {
		if (virtio_wlan_rate(&in->rx_rate, &sinfo->rxrate))
			sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BITRATE);
		else
			vw->stats.bad_rates++;
	}
}

static int virtio_wlan_get_station(struct wiphy *wiphy,
				   struct wireless_dev *wdev, const u8 *mac,
				   struct station_info *sinfo)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	struct virtio_wlan_get_station req = {};
	struct virtio_wlan_station_info info;
	size_t len;
	int err;

	ether_addr_copy(req.peer, mac);
	err = virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_GET_STATION, &req,
			      sizeof(req), &info, sizeof(info), &len);
	if (err)
		return err;
	if (len != sizeof(info)) {
		vw->stats.bad_responses++;
		return -EIO;
	}
	virtio_wlan_station_info(vw, &info, sinfo);
	return 0;
}

static int virtio_wlan_dump_station(struct wiphy *wiphy,
				    struct wireless_dev *wdev, int idx, u8 *mac,
				    struct station_info *sinfo)
{
	if (idx || !wdev->connected)
		return -ENOENT;
	memcpy(mac, wdev->u.client.connected_addr, ETH_ALEN);
	return virtio_wlan_get_station(wiphy, wdev, mac, sinfo);
}

/*
 * Report a BSS to cfg80211 with its IEs passed through the allowlist. A BSS
 * on a channel that the wiphy lacks or has disabled, or with an invalid
 * address or frame type, is refused.
 */
static struct cfg80211_bss *
virtio_wlan_inform_bss(struct virtio_wlan *vw,
		       const struct virtio_wlan_bss_info *info, const u8 *ies)
{
	size_t ie_len = le16_to_cpu(info->ie_len);
	struct cfg80211_inform_bss data = {};
	enum cfg80211_bss_frame_type ftype;
	struct cfg80211_bss *bss;
	size_t n;
	u8 *buf;

	data.chan = ieee80211_get_channel(vw->wiphy, le16_to_cpu(info->freq));
	if (!data.chan || (data.chan->flags & IEEE80211_CHAN_DISABLED) ||
	    !is_valid_ether_addr(info->bssid))
		return NULL;
	switch (info->frame_type) {
	case VIRTIO_WLAN_FRAME_BEACON:
		ftype = CFG80211_BSS_FTYPE_BEACON;
		break;
	case VIRTIO_WLAN_FRAME_PROBE_RESP:
		ftype = CFG80211_BSS_FTYPE_PRESP;
		break;
	default:
		return NULL;
	}

	buf = kmalloc(ie_len, GFP_KERNEL);
	if (!buf)
		return NULL;
	n = virtio_wlan_filter_elems(buf, ie_len, ies, ie_len, true, NULL);
	data.signal = (s32)le32_to_cpu(info->signal);
	data.boottime_ns = ktime_get_boottime_ns();
	bss = cfg80211_inform_bss_data(vw->wiphy, &data, ftype, info->bssid,
				       le64_to_cpu(info->tsf),
				       le16_to_cpu(info->capability),
				       le16_to_cpu(info->beacon_interval),
				       buf, n, GFP_KERNEL);
	kfree(buf);
	return bss;
}

static void virtio_wlan_event_bss(struct virtio_wlan *vw, const void *p,
				  size_t len)
{
	const struct virtio_wlan_event_bss *e = p;
	struct cfg80211_bss *bss;

	if (len < sizeof(*e) ||
	    le16_to_cpu(e->bss.ie_len) != len - sizeof(*e)) {
		vw->stats.bad_events++;
		return;
	}
	if (!vw->scan_req || le32_to_cpu(e->id) != vw->scan_id) {
		vw->stats.stale_events++;
		return;
	}

	bss = virtio_wlan_inform_bss(vw, &e->bss, (const u8 *)(e + 1));
	if (bss)
		cfg80211_put_bss(vw->wiphy, bss);
	else
		vw->stats.bad_bss++;
}

static void virtio_wlan_event_scan_done(struct virtio_wlan *vw, const void *p,
					size_t len)
{
	const struct virtio_wlan_event_scan_done *e = p;

	if (len < sizeof(*e)) {
		vw->stats.bad_events++;
		return;
	}
	if (!vw->scan_req || le32_to_cpu(e->id) != vw->scan_id) {
		vw->stats.stale_events++;
		return;
	}
	virtio_wlan_finish_scan(vw, e->aborted);
}

/*
 * Fail the current attempt, which the device may have completed, so it is
 * also told to disconnect.
 */
static void virtio_wlan_fail_connect(struct virtio_wlan *vw)
{
	struct cfg80211_connect_resp_params cr = {
		.status = WLAN_STATUS_UNSPECIFIED_FAILURE,
	};

	vw->stats.bad_connect_results++;
	virtio_wlan_drop_conn(vw);
	cfg80211_connect_done(vw->netdev, &cr, GFP_KERNEL);
}

/*
 * A hidden network's BSS can carry an empty or zeroed SSID. An SSID element
 * that the allowlist drops would leave a later one in the informed BSS.
 */
static bool virtio_wlan_ssid_matches(struct virtio_wlan *vw, const u8 *ies,
				     size_t len)
{
	const struct element *ssid = cfg80211_find_elem(WLAN_EID_SSID, ies, len);

	if (!ssid || ssid->datalen > IEEE80211_MAX_SSID_LEN)
		return false;
	if (!memchr_inv(ssid->data, 0, ssid->datalen))
		return true;
	return ssid->datalen == vw->conn_ssid_len &&
	       !memcmp(ssid->data, vw->conn_ssid, ssid->datalen);
}

static enum nl80211_timeout_reason virtio_wlan_timeout_reason(u8 reason)
{
	switch (reason) {
	case VIRTIO_WLAN_TIMEOUT_SCAN:
		return NL80211_TIMEOUT_SCAN;
	case VIRTIO_WLAN_TIMEOUT_AUTH:
		return NL80211_TIMEOUT_AUTH;
	case VIRTIO_WLAN_TIMEOUT_ASSOC:
		return NL80211_TIMEOUT_ASSOC;
	default:
		return NL80211_TIMEOUT_UNSPECIFIED;
	}
}

/*
 * A successful result must name a usable BSS of the requested network,
 * which is informed first, so that cfg80211 does not look it up.
 */
static void virtio_wlan_event_connect_result(struct virtio_wlan *vw,
					     const void *p, size_t len)
{
	const struct virtio_wlan_event_connect_result *e = p;
	struct cfg80211_connect_resp_params cr = {};
	size_t bss_len, req_len, resp_len;
	const u8 *bss_ies, *req_ies;
	bool success;
	u8 *buf;

	if (len < sizeof(*e)) {
		vw->stats.bad_events++;
		return;
	}
	if (vw->conn_state != VIRTIO_WLAN_CONNECTING ||
	    le32_to_cpu(e->id) != vw->conn_id) {
		vw->stats.stale_events++;
		return;
	}

	bss_len = le16_to_cpu(e->bss.ie_len);
	req_len = le16_to_cpu(e->req_ie_len);
	resp_len = le16_to_cpu(e->resp_ie_len);
	bss_ies = (const u8 *)(e + 1);
	req_ies = bss_ies + bss_len;
	if (bss_len + req_len + resp_len != len - sizeof(*e)) {
		virtio_wlan_fail_connect(vw);
		return;
	}

	success = !e->timeout_reason && !e->status;
	if (success) {
		if (virtio_wlan_ssid_matches(vw, bss_ies, bss_len))
			cr.links[0].bss = virtio_wlan_inform_bss(vw, &e->bss,
								 bss_ies);
		if (!cr.links[0].bss) {
			virtio_wlan_fail_connect(vw);
			return;
		}
		cr.links[0].bssid = cr.links[0].bss->bssid;
		vw->conn_state = VIRTIO_WLAN_CONNECTED;
		wiphy_delayed_work_cancel(vw->wiphy, &vw->conn_timeout);
	} else {
		virtio_wlan_end_conn(vw);
		if (is_valid_ether_addr(e->bss.bssid))
			cr.links[0].bssid = e->bss.bssid;
		if (e->timeout_reason) {
			cr.status = -1;
			cr.timeout_reason =
				virtio_wlan_timeout_reason(e->timeout_reason);
		} else {
			cr.status = le16_to_cpu(e->status);
		}
	}

	buf = kmalloc(req_len + resp_len, GFP_KERNEL);
	if (buf) {
		cr.req_ie = buf;
		cr.req_ie_len = virtio_wlan_filter_elems(buf, req_len, req_ies,
							 req_len, false, NULL);
		cr.resp_ie = buf + cr.req_ie_len;
		cr.resp_ie_len = virtio_wlan_filter_elems(buf + cr.req_ie_len,
							  resp_len,
							  req_ies + req_len,
							  resp_len, true, NULL);
	}
	cfg80211_connect_done(vw->netdev, &cr, GFP_KERNEL);
	kfree(buf);
}

/*
 * A disconnection during an attempt fails it, as a failed result would. One
 * that ends the connection is reported as it is.
 */
static void virtio_wlan_event_disconnected(struct virtio_wlan *vw,
					   const void *p, size_t len)
{
	const struct virtio_wlan_event_disconnected *e = p;
	struct cfg80211_connect_resp_params cr = {
		.status = WLAN_STATUS_UNSPECIFIED_FAILURE,
	};
	size_t ie_len;
	u8 *buf;

	if (len < sizeof(*e) ||
	    le16_to_cpu(e->ie_len) != len - sizeof(*e)) {
		vw->stats.bad_events++;
		return;
	}
	if (vw->conn_state == VIRTIO_WLAN_IDLE ||
	    le32_to_cpu(e->id) != vw->conn_id) {
		vw->stats.stale_events++;
		return;
	}

	if (virtio_wlan_end_conn(vw) == VIRTIO_WLAN_CONNECTING) {
		cfg80211_connect_done(vw->netdev, &cr, GFP_KERNEL);
		return;
	}

	ie_len = le16_to_cpu(e->ie_len);
	buf = kmalloc(ie_len, GFP_KERNEL);
	if (buf)
		ie_len = virtio_wlan_filter_elems(buf, ie_len,
						  (const u8 *)(e + 1), ie_len,
						  !e->locally_generated, NULL);
	cfg80211_disconnected(vw->netdev, le16_to_cpu(e->reason), buf,
			      buf ? ie_len : 0, e->locally_generated,
			      GFP_KERNEL);
	kfree(buf);
}

static void virtio_wlan_event_mic_failure(struct virtio_wlan *vw,
					  const void *p, size_t len)
{
	const struct virtio_wlan_event_mic_failure *e = p;
	int key_id = e->key_index;

	if (len < sizeof(*e) || !is_valid_ether_addr(e->peer) ||
	    (key_id > 3 && key_id != VIRTIO_WLAN_KEY_INDEX_NONE)) {
		vw->stats.bad_events++;
		return;
	}
	if (vw->conn_state != VIRTIO_WLAN_CONNECTED) {
		vw->stats.stale_events++;
		return;
	}
	if (key_id == VIRTIO_WLAN_KEY_INDEX_NONE)
		key_id = -1;
	cfg80211_michael_mic_failure(vw->netdev, e->peer,
				     e->pairwise ? NL80211_KEYTYPE_PAIRWISE :
						   NL80211_KEYTYPE_GROUP,
				     key_id, NULL, GFP_KERNEL);
}

static void virtio_wlan_handle_event(struct virtio_wlan *vw, const void *buf,
				     unsigned int len)
{
	const struct virtio_wlan_event_hdr *hdr = buf;
	size_t n;

	/* The used length is not trusted, so it is checked against the buffer. */
	if (len < sizeof(*hdr) || len > vw->max_event_size ||
	    le32_to_cpu(hdr->len) > len - sizeof(*hdr) || hdr->flags) {
		vw->stats.bad_events++;
		return;
	}
	n = le32_to_cpu(hdr->len);

	switch (le16_to_cpu(hdr->type)) {
	case VIRTIO_WLAN_EVENT_BSS:
		virtio_wlan_event_bss(vw, hdr + 1, n);
		break;
	case VIRTIO_WLAN_EVENT_SCAN_DONE:
		virtio_wlan_event_scan_done(vw, hdr + 1, n);
		break;
	case VIRTIO_WLAN_EVENT_CONNECT_RESULT:
		virtio_wlan_event_connect_result(vw, hdr + 1, n);
		break;
	case VIRTIO_WLAN_EVENT_DISCONNECTED:
		virtio_wlan_event_disconnected(vw, hdr + 1, n);
		break;
	case VIRTIO_WLAN_EVENT_MIC_FAILURE:
		virtio_wlan_event_mic_failure(vw, hdr + 1, n);
		break;
	default:
		vw->stats.bad_events++;
	}
}

static int virtio_wlan_post_event(struct virtio_wlan *vw, void *buf)
{
	struct scatterlist sg;

	sg_init_one(&sg, buf, vw->max_event_size);
	return virtqueue_add_inbuf(vw->event_vq, &sg, 1, buf, GFP_KERNEL);
}

static void virtio_wlan_schedule_events(struct virtio_wlan *vw,
					unsigned long delay)
{
	if (!test_and_set_bit(0, &vw->event_scheduled))
		wiphy_delayed_work_queue(vw->wiphy, &vw->event_work, delay);
}

/*
 * The callback only schedules the work. vring_interrupt() calls it whenever
 * used buffers are pending, even with callbacks disabled, so the scheduled
 * flag keeps it from queueing the work again.
 */
static void virtio_wlan_event_done(struct virtqueue *vq)
{
	struct virtio_wlan *vw = vq->vdev->priv;

	/* This pairs with the release in probe once the driver is complete. */
	if (smp_load_acquire(&vw->ready))
		virtio_wlan_schedule_events(vw, 0);
}

/*
 * Handle a bounded number of events and repost their buffers. A run that
 * handled any requeues itself a jiffy later, so that a device that keeps
 * completing buffers cannot keep the wiphy lock. Only a run that finds the
 * queue empty clears the scheduled flag and enables callbacks.
 */
static void virtio_wlan_event_work(struct wiphy *wiphy, struct wiphy_work *work)
{
	struct virtio_wlan *vw = wiphy_priv(wiphy);
	unsigned int n = 0, len;
	void *buf;

	if (vw->removing || vw->broken)
		return;

	virtqueue_disable_cb(vw->event_vq);
	while (n < VIRTIO_WLAN_EVENT_BUDGET && !vw->broken &&
	       (buf = virtqueue_get_buf(vw->event_vq, &len))) {
		virtio_wlan_handle_event(vw, buf, len);
		virtio_wlan_post_event(vw, buf);
		n++;
	}
	if (n) {
		virtqueue_kick(vw->event_vq);
		wiphy_delayed_work_queue(wiphy, &vw->event_work, 1);
		return;
	}

	clear_bit(0, &vw->event_scheduled);
	/*
	 * A buffer completed after the flag is clear finds it clear, and one
	 * completed before is seen by virtqueue_enable_cb().
	 */
	smp_mb__after_atomic();
	if (!virtqueue_enable_cb(vw->event_vq)) {
		virtqueue_disable_cb(vw->event_vq);
		virtio_wlan_schedule_events(vw, 1);
	}
}

static int virtio_wlan_open(struct net_device *dev)
{
	struct virtio_wlan *vw = wiphy_priv(dev->ieee80211_ptr->wiphy);

	guard(wiphy)(vw->wiphy);
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_OPEN, NULL, 0, NULL, 0, NULL);
}

/*
 * cfg80211 disconnects before the interface closes. An attempt that is still
 * tracked is dropped, and a pending scan is finished at once, since cfg80211
 * warns about a scan that outlives the interface.
 */
static int virtio_wlan_stop(struct net_device *dev)
{
	struct virtio_wlan *vw = wiphy_priv(dev->ieee80211_ptr->wiphy);

	guard(wiphy)(vw->wiphy);
	virtio_wlan_drop_conn(vw);
	if (vw->scan_req) {
		virtio_wlan_send_abort_scan(vw, vw->scan_id);
		virtio_wlan_finish_scan(vw, true);
	}
	virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_CLOSE, NULL, 0, NULL, 0, NULL);
	return 0;
}

static netdev_tx_t virtio_wlan_start_xmit(struct sk_buff *skb,
					  struct net_device *dev)
{
	dev_core_stats_tx_dropped_inc(dev);
	dev_kfree_skb_any(skb);
	return NETDEV_TX_OK;
}

static const struct net_device_ops virtio_wlan_netdev_ops = {
	.ndo_open = virtio_wlan_open,
	.ndo_stop = virtio_wlan_stop,
	.ndo_start_xmit = virtio_wlan_start_xmit,
	.ndo_validate_addr = eth_validate_addr,
};

static const struct cfg80211_ops virtio_wlan_cfg80211_ops = {
	.scan = virtio_wlan_scan,
	.abort_scan = virtio_wlan_abort_scan,
	.connect = virtio_wlan_connect,
	.disconnect = virtio_wlan_disconnect,
	.add_key = virtio_wlan_add_key,
	.del_key = virtio_wlan_del_key,
	.set_default_key = virtio_wlan_set_default_key,
	.set_default_mgmt_key = virtio_wlan_set_default_mgmt_key,
	.change_station = virtio_wlan_change_station,
	.get_station = virtio_wlan_get_station,
	.dump_station = virtio_wlan_dump_station,
};

static void virtio_wlan_netdev_setup(struct net_device *dev)
{
	ether_setup(dev);
	dev->netdev_ops = &virtio_wlan_netdev_ops;
}

static int virtio_wlan_read_config(struct virtio_device *vdev, u8 *mac,
				   u16 *mtu, u32 *max_event_size,
				   u32 *max_wiphy_size)
{
	virtio_cread_bytes(vdev, offsetof(struct virtio_wlan_config, mac), mac,
			   ETH_ALEN);
	virtio_cread_le(vdev, struct virtio_wlan_config, mtu, mtu);
	virtio_cread_le(vdev, struct virtio_wlan_config, max_event_size,
			max_event_size);
	virtio_cread_le(vdev, struct virtio_wlan_config, max_wiphy_size,
			max_wiphy_size);

	/* An event buffer holds at least the largest fixed event. */
	if (!is_valid_ether_addr(mac) || *mtu < ETH_MIN_MTU ||
	    *mtu > VIRTIO_WLAN_MAX_MTU ||
	    *max_event_size < sizeof(struct virtio_wlan_event_hdr) +
			      sizeof(struct virtio_wlan_event_connect_result) ||
	    *max_event_size > VIRTIO_WLAN_MAX_CONFIG_SIZE ||
	    *max_wiphy_size < sizeof(struct virtio_wlan_wiphy) ||
	    *max_wiphy_size > VIRTIO_WLAN_MAX_CONFIG_SIZE) {
		dev_err(&vdev->dev, "the configuration is invalid\n");
		return -EINVAL;
	}
	return 0;
}

static int virtio_wlan_add_event_bufs(struct virtio_wlan *vw)
{
	unsigned int i;
	int err;

	vw->n_event_bufs = min_t(unsigned int, VIRTIO_WLAN_EVENT_BUFS,
				 virtqueue_get_vring_size(vw->event_vq));
	vw->event_bufs = kcalloc(vw->n_event_bufs, sizeof(*vw->event_bufs),
				 GFP_KERNEL);
	if (!vw->event_bufs)
		return -ENOMEM;
	for (i = 0; i < vw->n_event_bufs; i++) {
		vw->event_bufs[i] = kmalloc(vw->max_event_size, GFP_KERNEL);
		if (!vw->event_bufs[i])
			return -ENOMEM;
		err = virtio_wlan_post_event(vw, vw->event_bufs[i]);
		if (err)
			return err;
	}
	virtqueue_kick(vw->event_vq);
	return 0;
}

/*
 * The device holds the posted event buffers, and the buffer of a command
 * that timed out, until reset.
 */
static void virtio_wlan_del_vqs(struct virtio_wlan *vw)
{
	virtio_reset_device(vw->vdev);
	while (virtqueue_detach_unused_buf(vw->cmd_vq))
		;
	while (virtqueue_detach_unused_buf(vw->event_vq))
		;
	vw->vdev->config->del_vqs(vw->vdev);
}

static void virtio_wlan_free(struct virtio_wlan *vw)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(vw->bands); i++)
		kfree(vw->bands[i].sband.channels);
	for (i = 0; i < vw->n_event_bufs; i++)
		kfree(vw->event_bufs[i]);
	kfree(vw->event_bufs);
	kfree(vw->resp_buf);
	kfree_sensitive(vw->cmd_buf);
}

static int virtio_wlan_probe(struct virtio_device *vdev)
{
	struct virtqueue_info vqs_info[VIRTIO_WLAN_VQ_MAX] = {
		[VIRTIO_WLAN_VQ_RX] = { "rx" },
		[VIRTIO_WLAN_VQ_TX] = { "tx" },
		[VIRTIO_WLAN_VQ_CMD] = { "cmd", virtio_wlan_cmd_done },
		[VIRTIO_WLAN_VQ_EVENT] = { "event", virtio_wlan_event_done },
	};
	struct virtqueue *vqs[VIRTIO_WLAN_VQ_MAX];
	u32 max_event_size, max_wiphy_size;
	struct net_device *netdev;
	struct virtio_wlan *vw;
	struct wiphy *wiphy;
	u8 mac[ETH_ALEN];
	u16 mtu;
	int i, err;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1) ||
	    !vdev->config->get)
		return -ENODEV;
	err = virtio_wlan_read_config(vdev, mac, &mtu, &max_event_size,
				      &max_wiphy_size);
	if (err)
		return err;

	wiphy = wiphy_new(&virtio_wlan_cfg80211_ops, sizeof(*vw));
	if (!wiphy)
		return -ENOMEM;
	vw = wiphy_priv(wiphy);
	vw->wiphy = wiphy;
	vw->vdev = vdev;
	ether_addr_copy(vw->mac, mac);
	vw->mtu = mtu;
	vw->max_event_size = max_event_size;
	vw->resp_size = max_wiphy_size;
	init_completion(&vw->cmd_done);
	wiphy_delayed_work_init(&vw->event_work, virtio_wlan_event_work);
	wiphy_delayed_work_init(&vw->scan_timeout, virtio_wlan_scan_timeout);
	wiphy_delayed_work_init(&vw->conn_timeout, virtio_wlan_conn_timeout);

	err = -ENOMEM;
	vw->cmd_size = sizeof(struct virtio_wlan_cmd_hdr);
	vw->cmd_buf = kmalloc(vw->cmd_size, GFP_KERNEL);
	vw->resp_buf = kmalloc(sizeof(*vw->resp_buf) + vw->resp_size,
			       GFP_KERNEL);
	if (!vw->cmd_buf || !vw->resp_buf)
		goto err_free;

	vdev->priv = vw;
	err = virtio_find_vqs(vdev, VIRTIO_WLAN_VQ_MAX, vqs, vqs_info, NULL);
	if (err)
		goto err_free;
	vw->cmd_vq = vqs[VIRTIO_WLAN_VQ_CMD];
	vw->event_vq = vqs[VIRTIO_WLAN_VQ_EVENT];

	/*
	 * A command takes two descriptors, and the ring warns when a request
	 * needs more than it has.
	 */
	if (virtqueue_get_vring_size(vw->cmd_vq) < 2) {
		dev_err(&vdev->dev, "the command queue is too small\n");
		err = -EINVAL;
		goto err_reset;
	}
	virtio_device_ready(vdev);

	wiphy_lock(wiphy);
	err = virtio_wlan_get_wiphy(vw);
	wiphy_unlock(wiphy);
	if (!err)
		err = virtio_wlan_add_event_bufs(vw);
	if (err)
		goto err_reset;

	set_wiphy_dev(wiphy, &vdev->dev);
	memcpy(wiphy->perm_addr, mac, ETH_ALEN);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	/* The device cannot lift restrictions through hints of its own. */
	wiphy->regulatory_flags = REGULATORY_COUNTRY_IE_IGNORE |
				  REGULATORY_DISABLE_BEACON_HINTS;
	err = wiphy_register(wiphy);
	if (err)
		goto err_reset;
	for (i = 0; i < ARRAY_SIZE(virtio_wlan_counters); i++)
		debugfs_create_u32(virtio_wlan_counters[i].name, 0444,
				   wiphy->debugfsdir,
				   (u32 *)((u8 *)&vw->stats +
					   virtio_wlan_counters[i].offset));

	netdev = alloc_netdev(0, "wlan%d", NET_NAME_ENUM,
			      virtio_wlan_netdev_setup);
	if (!netdev) {
		err = -ENOMEM;
		goto err_unregister;
	}
	vw->netdev = netdev;
	vw->wdev.wiphy = wiphy;
	vw->wdev.iftype = NL80211_IFTYPE_STATION;
	netdev->ieee80211_ptr = &vw->wdev;
	SET_NETDEV_DEV(netdev, &vdev->dev);
	eth_hw_addr_set(netdev, mac);
	netdev->mtu = mtu;
	netdev->max_mtu = mtu;
	netif_carrier_off(netdev);
	err = register_netdev(netdev);
	if (err)
		goto err_free_netdev;

	/*
	 * An event that arrived before this point found the flag clear, and
	 * its callback is not repeated, so the work runs once now.
	 */
	smp_store_release(&vw->ready, true);
	virtio_wlan_schedule_events(vw, 0);
	return 0;

err_free_netdev:
	free_netdev(netdev);
err_unregister:
	wiphy_unregister(wiphy);
err_reset:
	virtio_wlan_del_vqs(vw);
err_free:
	virtio_wlan_free(vw);
	wiphy_free(wiphy);
	return err;
}

static void virtio_wlan_remove(struct virtio_device *vdev)
{
	struct virtio_wlan *vw = vdev->priv;
	struct wiphy *wiphy = vw->wiphy;

	/*
	 * The works check the removing flag, and are cancelled with their
	 * timers, since wiphy_unregister() would otherwise run them after the
	 * interface is gone.
	 */
	wiphy_lock(wiphy);
	WRITE_ONCE(vw->ready, false);
	vw->removing = true;
	wiphy_delayed_work_cancel(wiphy, &vw->event_work);
	wiphy_delayed_work_cancel(wiphy, &vw->scan_timeout);
	wiphy_delayed_work_cancel(wiphy, &vw->conn_timeout);
	wiphy_unlock(wiphy);

	/* Unregistering closes the interface, which takes the wiphy lock. */
	unregister_netdev(vw->netdev);
	virtio_wlan_del_vqs(vw);
	wiphy_unregister(wiphy);
	free_netdev(vw->netdev);
	virtio_wlan_free(vw);
	wiphy_free(wiphy);
}

static const struct virtio_device_id virtio_wlan_id_table[] = {
	{ VIRTIO_ID_MAC80211_WLAN, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_wlan_driver = {
	.driver.name = KBUILD_MODNAME,
	.id_table = virtio_wlan_id_table,
	.probe = virtio_wlan_probe,
	.remove = virtio_wlan_remove,
};

module_virtio_driver(virtio_wlan_driver);
MODULE_DEVICE_TABLE(virtio, virtio_wlan_id_table);
MODULE_DESCRIPTION("Virtio WLAN driver");
MODULE_LICENSE("GPL");
