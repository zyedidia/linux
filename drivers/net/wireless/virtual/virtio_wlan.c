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
#define VIRTIO_WLAN_CMD_BUF_SIZE	4096
#define VIRTIO_WLAN_MAX_CONFIG_SIZE	16384
#define VIRTIO_WLAN_MAX_POWER		30
#define VIRTIO_WLAN_MAX_SCAN_SSIDS	16
#define VIRTIO_WLAN_MAX_SCAN_IE_LEN	2048

/* A received frame fits in one page together with both of its headers. */
#define VIRTIO_WLAN_MAX_MTU \
	(PAGE_SIZE - sizeof(struct virtio_net_hdr_v1) - ETH_HLEN)

static_assert(VIRTIO_WLAN_MAX_AKM_SUITES <= CFG80211_MAX_NUM_AKM_SUITES);
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
};

struct virtio_wlan {
	struct wiphy *wiphy;
	struct virtio_device *vdev;
	struct net_device *netdev;
	struct wireless_dev wdev;
	struct virtqueue *cmd_vq;
	struct completion cmd_done;
	void *cmd_buf;
	struct virtio_wlan_resp *resp_buf;
	size_t resp_size;
	u8 mac[ETH_ALEN];
	u16 mtu;
	u32 max_event_size;
	struct virtio_wlan_band_data bands[ARRAY_SIZE(virtio_wlan_bands)];
	u32 ciphers[VIRTIO_WLAN_MAX_CIPHER_SUITES];
	/* These are changed under the wiphy lock. */
	bool removing;
	bool broken;
	struct virtio_wlan_stats stats;
};

static void virtio_wlan_cmd_done(struct virtqueue *vq)
{
	struct virtio_wlan *vw = vq->vdev->priv;

	complete(&vw->cmd_done);
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
	if (WARN_ON(req_len > VIRTIO_WLAN_CMD_BUF_SIZE - sizeof(*hdr) ||
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
	if (err)
		return err;

	deadline = jiffies + VIRTIO_WLAN_CMD_TIMEOUT;
	virtqueue_kick(vw->cmd_vq);
	for (;;) {
		if (virtqueue_get_buf(vw->cmd_vq, &len))
			break;
		left = (long)(deadline - jiffies);
		if (virtqueue_is_broken(vw->cmd_vq) || left <= 0) {
			vw->stats.cmd_timeouts++;
			vw->broken = true;
			dev_err(&vw->vdev->dev, "command %u got no response\n",
				type);
			return -ETIMEDOUT;
		}
		wait_for_completion_timeout(&vw->cmd_done, left);
	}

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
	return err;
}

static int virtio_wlan_open(struct net_device *dev)
{
	struct virtio_wlan *vw = wiphy_priv(dev->ieee80211_ptr->wiphy);

	guard(wiphy)(vw->wiphy);
	return virtio_wlan_cmd(vw, VIRTIO_WLAN_CMD_OPEN, NULL, 0, NULL, 0, NULL);
}

static int virtio_wlan_stop(struct net_device *dev)
{
	struct virtio_wlan *vw = wiphy_priv(dev->ieee80211_ptr->wiphy);

	guard(wiphy)(vw->wiphy);
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

static const struct cfg80211_ops virtio_wlan_cfg80211_ops = {};

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

/* A command that timed out leaves its buffer with the device until reset. */
static void virtio_wlan_del_vqs(struct virtio_wlan *vw)
{
	virtio_reset_device(vw->vdev);
	while (virtqueue_detach_unused_buf(vw->cmd_vq))
		;
	vw->vdev->config->del_vqs(vw->vdev);
}

static void virtio_wlan_free(struct virtio_wlan *vw)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vw->bands); i++)
		kfree(vw->bands[i].sband.channels);
	kfree(vw->resp_buf);
	kfree(vw->cmd_buf);
}

static int virtio_wlan_probe(struct virtio_device *vdev)
{
	struct virtqueue_info vqs_info[VIRTIO_WLAN_VQ_MAX] = {
		[VIRTIO_WLAN_VQ_RX] = { "rx" },
		[VIRTIO_WLAN_VQ_TX] = { "tx" },
		[VIRTIO_WLAN_VQ_CMD] = { "cmd", virtio_wlan_cmd_done },
		[VIRTIO_WLAN_VQ_EVENT] = { "event" },
	};
	struct virtqueue *vqs[VIRTIO_WLAN_VQ_MAX];
	u32 max_event_size, max_wiphy_size;
	struct net_device *netdev;
	struct virtio_wlan *vw;
	struct wiphy *wiphy;
	u8 mac[ETH_ALEN];
	u16 mtu;
	int err;

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

	err = -ENOMEM;
	vw->cmd_buf = kmalloc(VIRTIO_WLAN_CMD_BUF_SIZE, GFP_KERNEL);
	vw->resp_buf = kmalloc(sizeof(*vw->resp_buf) + vw->resp_size,
			       GFP_KERNEL);
	if (!vw->cmd_buf || !vw->resp_buf)
		goto err_free;

	vdev->priv = vw;
	err = virtio_find_vqs(vdev, VIRTIO_WLAN_VQ_MAX, vqs, vqs_info, NULL);
	if (err)
		goto err_free;
	vw->cmd_vq = vqs[VIRTIO_WLAN_VQ_CMD];

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
	debugfs_create_u32("cmd_timeouts", 0444, wiphy->debugfsdir,
			   &vw->stats.cmd_timeouts);
	debugfs_create_u32("bad_responses", 0444, wiphy->debugfsdir,
			   &vw->stats.bad_responses);

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

	wiphy_lock(wiphy);
	vw->removing = true;
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
