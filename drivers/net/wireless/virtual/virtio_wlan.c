// SPDX-License-Identifier: GPL-2.0
/*
 * virtio-wlan: a cfg80211 driver for a virtio WLAN device.
 *
 * The device implements association (a fullmac-style split): cfg80211
 * requests are serialised as commands on the command virtqueue and
 * results arrive as events, while the data queues carry 802.3 frames.
 * EAPOL therefore reaches the ordinary supplicant on this side, which
 * installs the resulting keys with VIRTIO_WLAN_CMD_ADD_KEY.
 *
 * The device implementation is not trusted. Three rules follow from
 * that and are relied on throughout:
 *
 *  - Nothing the device says is believed without a bound: buffer
 *    lengths reported through the used ring, event contents, and the
 *    device's own description of itself are all validated.
 *  - The device cannot make the driver touch state that does not exist
 *    yet: the data and event callbacks are gated on ->ready, which is
 *    published only once probe has built everything they use.
 *  - The device cannot make the driver wait forever: commands time out
 *    and mark the device broken, and a scan that is never completed is
 *    finished by a timer.
 */

#define pr_fmt(fmt) "virtio-wlan: " fmt

#include <linux/etherdevice.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ids.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <uapi/linux/virtio_wlan.h>

/*
 * The wire layout must not acquire implicit padding: both sides access
 * these structures in place, and any 64-bit member has to stay
 * naturally aligned once the eight-byte command or event header is
 * accounted for.
 */
static_assert(sizeof(struct virtio_wlan_cmd_hdr) == 8);
static_assert(sizeof(struct virtio_wlan_cmd_resp) == 8);
static_assert(sizeof(struct virtio_wlan_event_hdr) == 8);
static_assert(sizeof(struct virtio_wlan_wiphy_info) == 28);
static_assert(sizeof(struct virtio_wlan_channel) == 16);
static_assert(sizeof(struct virtio_wlan_ssid) == 36);
static_assert(sizeof(struct virtio_wlan_cmd_scan) == 16);
static_assert(sizeof(struct virtio_wlan_cmd_connect) == 136);
static_assert(sizeof(struct virtio_wlan_cmd_add_key) == 64);
static_assert(sizeof(struct virtio_wlan_cmd_del_key) == 12);
static_assert(sizeof(struct virtio_wlan_cmd_set_port_authorized) == 8);
static_assert(sizeof(struct virtio_wlan_cmd_set_default_key) == 4);
static_assert(sizeof(struct virtio_wlan_cmd_set_default_mgmt_key) == 4);
static_assert(sizeof(struct virtio_wlan_station_info) == 56);
static_assert(sizeof(struct virtio_wlan_ev_scan_result) == 32);
static_assert(sizeof(struct virtio_wlan_ev_connect_result) == 16);
static_assert(sizeof(struct virtio_wlan_ev_disconnected) == 8);
static_assert(offsetof(struct virtio_wlan_ev_scan_result, tsf) == 0);
static_assert(offsetof(struct virtio_wlan_station_info, rx_packets) % 8 == 0);

#define VWLAN_CMD_BUF_SIZE	4096
#define VWLAN_RESP_BUF_SIZE	8192
#define VWLAN_EVENT_BUF_SIZE	2048
#define VWLAN_NUM_EVENT_BUFS	16
#define VWLAN_RX_BUF_SIZE	2048
#define VWLAN_CMD_TIMEOUT	msecs_to_jiffies(5000)
#define VWLAN_SCAN_TIMEOUT	msecs_to_jiffies(15000)

/* Bounds on what the device may declare about itself. */
#define VWLAN_MAX_CHANNELS	256
#define VWLAN_MAX_CIPHER_SUITES	32
#define VWLAN_MAX_SCAN_SSIDS	16
/*
 * ieee80211_get_channel() takes an int and multiplies by 1000, so a
 * device-supplied frequency has to stay well clear of overflowing it.
 */
#define VWLAN_MAX_FREQ_MHZ	100000

struct virtio_wlan {
	struct virtio_device *vdev;
	struct wiphy *wiphy;
	struct net_device *netdev;
	struct wireless_dev wdev;

	struct virtqueue *rx_vq;
	struct virtqueue *tx_vq;
	struct virtqueue *cmd_vq;
	struct virtqueue *event_vq;

	/*
	 * Published with smp_store_release() at the end of probe and
	 * cleared first thing in teardown. The data and event callbacks
	 * refuse to run unless it is set, which is what stops a device
	 * that raises interrupts early from reaching a NULL netdev, an
	 * unregistered wireless_dev or an uninitialised NAPI instance.
	 */
	bool ready;

	/* Command path: one command at a time, guarded by cmd_lock. */
	struct mutex cmd_lock;
	struct completion cmd_done;
	void *cmd_req;
	void *cmd_resp;
	bool broken;

	/* Event path. */
	void *event_bufs[VWLAN_NUM_EVENT_BUFS];
	int n_event_bufs;
	struct work_struct event_work;

	/* Data path. */
	struct napi_struct napi;
	spinlock_t tx_lock;

	/*
	 * Retries a receive-buffer allocation that failed under
	 * GFP_ATOMIC. Enabled only while the interface is up, so that it
	 * can never run against a disabled NAPI instance.
	 */
	struct delayed_work refill;
	bool refill_enabled;
	spinlock_t refill_lock;

	/* Driver-side view of device state. */
	struct mutex state_lock;
	struct cfg80211_scan_request *scan_req;
	unsigned long scan_started;
	struct delayed_work scan_timeout;
	bool connecting;
	bool connected;
	u8 connect_ssid[VIRTIO_WLAN_MAX_SSID_LEN];
	u8 connect_ssid_len;

	/* Bands built from the device's channel list. */
	struct ieee80211_supported_band bands[NUM_NL80211_BANDS];
	struct ieee80211_channel *channels[NUM_NL80211_BANDS];
	u32 *cipher_suites;
	struct ieee80211_rate rates_2ghz[12];
	struct ieee80211_rate rates_ofdm[8];
};

#define VWLAN_RATE(_rate, _flags) {			\
	.bitrate	= (_rate),			\
	.hw_value	= (_rate),			\
	.flags		= (_flags),			\
}

/*
 * Legacy rate templates. Association happens on the device side, so
 * these only describe the band to cfg80211. They are copied per
 * instance because wiphy_register() writes mandatory-rate flags into
 * the band's rate array.
 */
static const struct ieee80211_rate vwlan_rate_template[] = {
	VWLAN_RATE(10, 0),
	VWLAN_RATE(20, IEEE80211_RATE_SHORT_PREAMBLE),
	VWLAN_RATE(55, IEEE80211_RATE_SHORT_PREAMBLE),
	VWLAN_RATE(110, IEEE80211_RATE_SHORT_PREAMBLE),
	VWLAN_RATE(60, 0),
	VWLAN_RATE(90, 0),
	VWLAN_RATE(120, 0),
	VWLAN_RATE(180, 0),
	VWLAN_RATE(240, 0),
	VWLAN_RATE(360, 0),
	VWLAN_RATE(480, 0),
	VWLAN_RATE(540, 0),
};

#define VWLAN_OFDM_OFFSET	4

static_assert(ARRAY_SIZE(vwlan_rate_template) == 12);

static int vwlan_status_to_errno(u16 status)
{
	switch (status) {
	case VIRTIO_WLAN_S_OK:
		return 0;
	case VIRTIO_WLAN_S_EINVAL:
		return -EINVAL;
	case VIRTIO_WLAN_S_ENOTSUPP:
		return -EOPNOTSUPP;
	case VIRTIO_WLAN_S_EBUSY:
		return -EBUSY;
	case VIRTIO_WLAN_S_ENOENT:
		return -ENOENT;
	case VIRTIO_WLAN_S_ENOMEM:
		return -ENOMEM;
	default:
		return -EIO;
	}
}

/* Command path */

/*
 * The callback only signals; the response is reclaimed by the waiter.
 * Keeping virtqueue_get_buf() out of interrupt context means the
 * command virtqueue is only ever manipulated from the one process
 * context that holds cmd_lock, so a device that injects a spurious
 * notification cannot race the descriptor bookkeeping of a command
 * that is still being submitted.
 */
static void vwlan_cmd_cb(struct virtqueue *vq)
{
	struct virtio_wlan *priv = vq->vdev->priv;

	complete(&priv->cmd_done);
}

/*
 * Issue one command and wait for its response. @resp receives the
 * response payload (the part after struct virtio_wlan_cmd_resp).
 */
static int vwlan_cmd(struct virtio_wlan *priv, u16 cmd,
		     const void *req, size_t req_len,
		     void *resp, size_t resp_size, size_t *resp_len)
{
	struct virtio_wlan_cmd_hdr *hdr = priv->cmd_req;
	struct virtio_wlan_cmd_resp *rsp = priv->cmd_resp;
	struct scatterlist sg[2], *sgs[2];
	unsigned long deadline;
	unsigned int len = 0;
	size_t payload_len;
	int ret;

	if (sizeof(*hdr) + req_len > VWLAN_CMD_BUF_SIZE)
		return -EINVAL;

	guard(mutex)(&priv->cmd_lock);

	if (priv->broken)
		return -EIO;

	hdr->cmd = cpu_to_le16(cmd);
	hdr->flags = 0;
	hdr->reserved = 0;
	if (req_len)
		memcpy(hdr + 1, req, req_len);
	memset(rsp, 0, sizeof(*rsp));

	sg_init_one(&sg[0], priv->cmd_req, sizeof(*hdr) + req_len);
	sg_init_one(&sg[1], priv->cmd_resp, VWLAN_RESP_BUF_SIZE);
	sgs[0] = &sg[0];
	sgs[1] = &sg[1];

	reinit_completion(&priv->cmd_done);

	ret = virtqueue_add_sgs(priv->cmd_vq, sgs, 1, 1, priv, GFP_KERNEL);
	if (ret)
		return ret;
	virtqueue_kick(priv->cmd_vq);

	/*
	 * Wait against a real deadline rather than a completion count:
	 * the device can signal the completion as often as it likes, and
	 * must not be able to extend the wait by doing so.
	 */
	deadline = jiffies + VWLAN_CMD_TIMEOUT;
	for (;;) {
		long left;

		if (virtqueue_get_buf(priv->cmd_vq, &len))
			break;

		left = (long)(deadline - jiffies);
		if (left <= 0) {
			/*
			 * The command buffers are still owned by the
			 * device (or were reclaimed by a notification we
			 * never matched), so they must not be reused.
			 * Fail every later command instead of letting a
			 * wedged device stall each caller in turn.
			 */
			priv->broken = true;
			dev_err(&priv->vdev->dev,
				"command %u timed out, device marked broken\n",
				cmd);
			return -ETIMEDOUT;
		}
		wait_for_completion_timeout(&priv->cmd_done, left);

		/* A device that signals without completing anything must
		 * not be able to spin this loop on a CPU. */
		cond_resched();
	}

	/* The used length is device-supplied; virtio does not bound it. */
	if (len < sizeof(*rsp) || len > VWLAN_RESP_BUF_SIZE)
		return -EIO;
	payload_len = len - sizeof(*rsp);

	ret = vwlan_status_to_errno(le16_to_cpu(rsp->status));
	if (ret)
		return ret;

	if (resp) {
		if (payload_len > resp_size)
			payload_len = resp_size;
		memcpy(resp, rsp + 1, payload_len);
	}
	if (resp_len)
		*resp_len = payload_len;

	return 0;
}

/* Scan bookkeeping */

/*
 * Complete the outstanding scan, if there is one. state_lock is held
 * across cfg80211_scan_done() so that two finishers (the event, the
 * timeout and ndo_stop can all race) cannot complete the same request
 * twice. That is safe against the ops path, which runs under the wiphy
 * mutex and then takes state_lock, because cfg80211_scan_done() only
 * queues wiphy work under a spinlock and never takes the wiphy mutex.
 */
static void vwlan_scan_finish(struct virtio_wlan *priv, bool aborted)
{
	struct cfg80211_scan_info info = { .aborted = aborted };

	guard(mutex)(&priv->state_lock);

	if (!priv->scan_req)
		return;

	cfg80211_scan_done(priv->scan_req, &info);
	priv->scan_req = NULL;
}

static void vwlan_scan_timeout(struct work_struct *work)
{
	struct virtio_wlan *priv = container_of(to_delayed_work(work),
						struct virtio_wlan,
						scan_timeout);
	struct cfg80211_scan_info info = { .aborted = true };
	unsigned long expiry;
	bool aborted = false;

	scoped_guard(mutex, &priv->state_lock) {
		if (!priv->scan_req)
			return;

		/*
		 * This may be a timer armed for an earlier scan that
		 * completed just as it fired: only abort a scan that has
		 * really run out of time, and re-arm for whatever the
		 * current one has left. That also covers the case where
		 * starting a scan could not queue the timer because this
		 * stale one was still pending.
		 */
		expiry = priv->scan_started + VWLAN_SCAN_TIMEOUT;
		if (time_before(jiffies, expiry)) {
			schedule_delayed_work(&priv->scan_timeout,
					      expiry - jiffies);
			return;
		}

		dev_warn(&priv->vdev->dev,
			 "scan not completed by device, aborting\n");
		cfg80211_scan_done(priv->scan_req, &info);
		priv->scan_req = NULL;
		aborted = true;
	}

	/*
	 * Tell the device too, outside the lock. Otherwise it keeps
	 * scanning and a later completion would be attributed to
	 * whichever scan is outstanding by then.
	 */
	if (aborted)
		vwlan_cmd(priv, VIRTIO_WLAN_CMD_ABORT_SCAN, NULL, 0, NULL, 0,
			  NULL);
}

/* Event path */

static void vwlan_ev_scan_result(struct virtio_wlan *priv, const void *p,
				 unsigned int len)
{
	const struct virtio_wlan_ev_scan_result *ev = p;
	struct cfg80211_inform_bss data = {};
	enum cfg80211_bss_frame_type ftype;
	struct ieee80211_channel *chan;
	struct cfg80211_bss *bss;
	u32 ie_len, freq;

	if (len < sizeof(*ev))
		return;

	ie_len = le32_to_cpu(ev->ie_len);
	if (ie_len > len - sizeof(*ev))
		return;

	freq = le32_to_cpu(ev->center_freq);
	if (freq > VWLAN_MAX_FREQ_MHZ)
		return;

	chan = ieee80211_get_channel(priv->wiphy, freq);
	if (!chan || chan->flags & IEEE80211_CHAN_DISABLED)
		return;

	switch (ev->ftype) {
	case VIRTIO_WLAN_FTYPE_BEACON:
		ftype = CFG80211_BSS_FTYPE_BEACON;
		break;
	case VIRTIO_WLAN_FTYPE_PRESP:
		ftype = CFG80211_BSS_FTYPE_PRESP;
		break;
	default:
		ftype = CFG80211_BSS_FTYPE_UNKNOWN;
		break;
	}

	data.chan = chan;
	data.signal = (s32)le32_to_cpu(ev->signal);
	data.boottime_ns = ktime_get_boottime_ns();

	bss = cfg80211_inform_bss_data(priv->wiphy, &data, ftype, ev->bssid,
				       le64_to_cpu(ev->tsf),
				       le16_to_cpu(ev->capability),
				       le16_to_cpu(ev->beacon_interval),
				       (const u8 *)(ev + 1), ie_len,
				       GFP_KERNEL);
	if (bss)
		cfg80211_put_bss(priv->wiphy, bss);
}

static void vwlan_ev_scan_done(struct virtio_wlan *priv, const void *p,
			       unsigned int len)
{
	const struct virtio_wlan_ev_scan_done *ev = p;

	if (len < sizeof(*ev))
		return;

	cancel_delayed_work(&priv->scan_timeout);
	vwlan_scan_finish(priv, !!ev->aborted);
}

static void vwlan_ev_connect_result(struct virtio_wlan *priv, const void *p,
				    unsigned int len)
{
	const struct virtio_wlan_ev_connect_result *ev = p;
	struct cfg80211_connect_resp_params params = {};
	struct cfg80211_bss *bss = NULL;
	u32 req_ie_len, resp_ie_len;
	u16 status;
	const u8 *ies;

	if (len < sizeof(*ev))
		return;

	ies = (const u8 *)(ev + 1);
	req_ie_len = le32_to_cpu(ev->req_ie_len);
	resp_ie_len = le32_to_cpu(ev->resp_ie_len);
	if (req_ie_len > len - sizeof(*ev))
		return;
	if (resp_ie_len > len - sizeof(*ev) - req_ie_len)
		return;

	status = le16_to_cpu(ev->status);

	/*
	 * Held across the whole handler: ndo_stop() must not be able to
	 * tear the connection down between deciding to report a result
	 * and reporting it. None of the cfg80211 helpers used here take
	 * the wiphy mutex, so this cannot invert against the ops path.
	 */
	guard(mutex)(&priv->state_lock);

	/* Nothing asked for this; cfg80211 would warn. */
	if (!priv->connecting)
		return;
	priv->connecting = false;

	/*
	 * Look the BSS up and keep the reference: cfg80211 would
	 * otherwise repeat the lookup when it processes the event, by
	 * which time the entry may have been expired or evicted, and it
	 * warns when it cannot find one.
	 */
	if (status == WLAN_STATUS_SUCCESS) {
		bss = cfg80211_get_bss(priv->wiphy, NULL, ev->bssid,
				       priv->connect_ssid,
				       priv->connect_ssid_len,
				       IEEE80211_BSS_TYPE_ESS,
				       IEEE80211_PRIVACY_ANY);
		if (!bss) {
			dev_warn_ratelimited(&priv->vdev->dev,
					     "connect result for unknown BSS %pM\n",
					     ev->bssid);
			status = WLAN_STATUS_UNSPECIFIED_FAILURE;
		}
	}

	priv->connected = status == WLAN_STATUS_SUCCESS;

	params.status = status;
	params.links[0].bssid = ev->bssid;
	/* The reference is consumed by cfg80211. */
	params.links[0].bss = bss;
	if (req_ie_len) {
		params.req_ie = ies;
		params.req_ie_len = req_ie_len;
	}
	if (resp_ie_len) {
		params.resp_ie = ies + req_ie_len;
		params.resp_ie_len = resp_ie_len;
	}

	if (priv->connected)
		netif_carrier_on(priv->netdev);

	cfg80211_connect_done(priv->netdev, &params, GFP_KERNEL);
}

static void vwlan_ev_disconnected(struct virtio_wlan *priv, const void *p,
				  unsigned int len)
{
	const struct virtio_wlan_ev_disconnected *ev = p;
	u32 ie_len;

	if (len < sizeof(*ev))
		return;

	ie_len = le32_to_cpu(ev->ie_len);
	if (ie_len > len - sizeof(*ev))
		return;

	scoped_guard(mutex, &priv->state_lock) {
		if (!priv->connecting && !priv->connected)
			return;
		priv->connecting = false;
		priv->connected = false;
	}

	netif_carrier_off(priv->netdev);
	/* The parameter is locally_generated, i.e. the opposite of from_ap. */
	cfg80211_disconnected(priv->netdev, le16_to_cpu(ev->reason),
			      ie_len ? (const u8 *)(ev + 1) : NULL, ie_len,
			      !ev->from_ap, GFP_KERNEL);
}

static void vwlan_ev_mic_failure(struct virtio_wlan *priv, const void *p,
				 unsigned int len)
{
	const struct virtio_wlan_ev_mic_failure *ev = p;
	bool connected;

	if (len < sizeof(*ev))
		return;

	/* nl80211 carries the key id as 0..3. */
	if (ev->key_idx > 3)
		return;

	scoped_guard(mutex, &priv->state_lock)
		connected = priv->connected;
	if (!connected)
		return;

	cfg80211_michael_mic_failure(priv->netdev, ev->mac,
				     ev->pairwise ? NL80211_KEYTYPE_PAIRWISE :
						    NL80211_KEYTYPE_GROUP,
				     ev->key_idx, NULL, GFP_KERNEL);
}

static void vwlan_handle_event(struct virtio_wlan *priv, const void *buf,
			       unsigned int len)
{
	const struct virtio_wlan_event_hdr *hdr = buf;
	unsigned int payload_len;
	const void *payload;

	if (len < sizeof(*hdr) || len > VWLAN_EVENT_BUF_SIZE)
		return;

	payload = hdr + 1;
	payload_len = len - sizeof(*hdr);
	if (le16_to_cpu(hdr->len) > payload_len)
		return;
	payload_len = le16_to_cpu(hdr->len);

	switch (le16_to_cpu(hdr->type)) {
	case VIRTIO_WLAN_EV_SCAN_RESULT:
		vwlan_ev_scan_result(priv, payload, payload_len);
		break;
	case VIRTIO_WLAN_EV_SCAN_DONE:
		vwlan_ev_scan_done(priv, payload, payload_len);
		break;
	case VIRTIO_WLAN_EV_CONNECT_RESULT:
		vwlan_ev_connect_result(priv, payload, payload_len);
		break;
	case VIRTIO_WLAN_EV_DISCONNECTED:
		vwlan_ev_disconnected(priv, payload, payload_len);
		break;
	case VIRTIO_WLAN_EV_MIC_FAILURE:
		vwlan_ev_mic_failure(priv, payload, payload_len);
		break;
	default:
		break;
	}
}

static int vwlan_post_event_buf(struct virtio_wlan *priv, void *buf, gfp_t gfp)
{
	struct scatterlist sg;

	sg_init_one(&sg, buf, VWLAN_EVENT_BUF_SIZE);
	return virtqueue_add_inbuf(priv->event_vq, &sg, 1, buf, gfp);
}

static void vwlan_event_work(struct work_struct *work)
{
	struct virtio_wlan *priv = container_of(work, struct virtio_wlan,
						event_work);
	unsigned int len;
	void *buf;

	if (!smp_load_acquire(&priv->ready))
		return;

	do {
		virtqueue_disable_cb(priv->event_vq);

		while ((buf = virtqueue_get_buf(priv->event_vq, &len))) {
			vwlan_handle_event(priv, buf, len);
			if (vwlan_post_event_buf(priv, buf, GFP_KERNEL))
				dev_warn_ratelimited(&priv->vdev->dev,
						     "cannot repost event buffer\n");
		}
		virtqueue_kick(priv->event_vq);
	} while (!virtqueue_enable_cb(priv->event_vq));
}

static void vwlan_event_cb(struct virtqueue *vq)
{
	struct virtio_wlan *priv = vq->vdev->priv;

	if (!smp_load_acquire(&priv->ready))
		return;

	schedule_work(&priv->event_work);
}

/* Data path */

static int vwlan_add_rx_buf(struct virtio_wlan *priv, gfp_t gfp)
{
	struct scatterlist sg;
	struct sk_buff *skb;
	int err;

	skb = __netdev_alloc_skb_ip_align(priv->netdev, VWLAN_RX_BUF_SIZE, gfp);
	if (!skb)
		return -ENOMEM;

	sg_init_one(&sg, skb->data, VWLAN_RX_BUF_SIZE);
	err = virtqueue_add_inbuf(priv->rx_vq, &sg, 1, skb, gfp);
	if (err)
		dev_kfree_skb_any(skb);

	return err;
}

/*
 * Returns false if the receive queue ended up completely empty, which
 * is unrecoverable on its own: with no buffer for the device to
 * complete, no interrupt and therefore no poll will ever follow.
 */
static bool vwlan_fill_rx(struct virtio_wlan *priv, gfp_t gfp)
{
	bool kick = false;
	int err = 0;

	while (priv->rx_vq->num_free) {
		err = vwlan_add_rx_buf(priv, gfp);
		if (err)
			break;
		kick = true;
	}

	if (kick)
		virtqueue_kick(priv->rx_vq);

	/*
	 * A broken queue is not worth retrying: the device is gone, so
	 * report success and let the poll finish rather than spin.
	 */
	if (err == -EIO)
		return true;

	return priv->rx_vq->num_free < virtqueue_get_vring_size(priv->rx_vq);
}

static void vwlan_enable_refill(struct virtio_wlan *priv)
{
	guard(spinlock_bh)(&priv->refill_lock);
	priv->refill_enabled = true;
}

static void vwlan_disable_refill(struct virtio_wlan *priv)
{
	guard(spinlock_bh)(&priv->refill_lock);
	priv->refill_enabled = false;
}

static void vwlan_schedule_refill(struct virtio_wlan *priv, unsigned long delay)
{
	guard(spinlock)(&priv->refill_lock);
	if (priv->refill_enabled)
		schedule_delayed_work(&priv->refill, delay);
}

/*
 * Retry the allocation from process context with GFP_KERNEL. NAPI is
 * parked across the refill because the poll would otherwise touch the
 * receive queue at the same time.
 */
static void vwlan_refill_work(struct work_struct *work)
{
	struct virtio_wlan *priv = container_of(to_delayed_work(work),
						struct virtio_wlan, refill);
	bool filled;

	napi_disable(&priv->napi);
	filled = vwlan_fill_rx(priv, GFP_KERNEL);
	napi_enable(&priv->napi);

	local_bh_disable();
	napi_schedule(&priv->napi);
	local_bh_enable();

	if (!filled)
		vwlan_schedule_refill(priv, HZ / 2);
}

static int vwlan_poll(struct napi_struct *napi, int budget)
{
	struct virtio_wlan *priv = container_of(napi, struct virtio_wlan, napi);
	struct net_device *dev = priv->netdev;
	struct sk_buff *skb;
	unsigned int len;
	int received = 0;

	while (received < budget &&
	       (skb = virtqueue_get_buf(priv->rx_vq, &len))) {
		if (len < ETH_HLEN || len > VWLAN_RX_BUF_SIZE) {
			dev->stats.rx_length_errors++;
			dev_kfree_skb_any(skb);
		} else {
			skb_put(skb, len);
			skb->protocol = eth_type_trans(skb, dev);
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += len;
			napi_gro_receive(napi, skb);
		}
		received++;
	}

	/*
	 * An empty receive queue is unrecoverable on its own: the device
	 * has nothing to complete, so no interrupt and no further poll
	 * would follow. Hand the retry to process context rather than
	 * spinning here on budget.
	 */
	if (!vwlan_fill_rx(priv, GFP_ATOMIC))
		vwlan_schedule_refill(priv, 0);

	if (received < budget) {
		unsigned int opaque = virtqueue_enable_cb_prepare(priv->rx_vq);

		if (napi_complete_done(napi, received)) {
			/*
			 * virtqueue_poll() reports the race where the
			 * device completed something between the drain
			 * above and re-arming the callback; only then is
			 * another poll needed.
			 */
			if (unlikely(virtqueue_poll(priv->rx_vq, opaque)) &&
			    napi_schedule_prep(napi)) {
				virtqueue_disable_cb(priv->rx_vq);
				__napi_schedule(napi);
			}
		} else {
			virtqueue_disable_cb(priv->rx_vq);
		}
	}

	return received;
}

static void vwlan_rx_cb(struct virtqueue *vq)
{
	struct virtio_wlan *priv = vq->vdev->priv;

	if (!smp_load_acquire(&priv->ready))
		return;

	/*
	 * Only silence the callback once the poll has actually been
	 * taken: napi_schedule_prep() fails whenever NAPI is disabled,
	 * and an unconditional disable would then never be undone,
	 * because only vwlan_poll() re-enables it.
	 */
	if (napi_schedule_prep(&priv->napi)) {
		virtqueue_disable_cb(vq);
		__napi_schedule(&priv->napi);
	}
}

/* Caller holds tx_lock. */
static void vwlan_free_old_tx(struct virtio_wlan *priv)
{
	struct sk_buff *skb;
	unsigned int len;

	while ((skb = virtqueue_get_buf(priv->tx_vq, &len)))
		dev_consume_skb_any(skb);
}

static void vwlan_tx_cb(struct virtqueue *vq)
{
	struct virtio_wlan *priv = vq->vdev->priv;

	if (!smp_load_acquire(&priv->ready))
		return;

	guard(spinlock_irqsave)(&priv->tx_lock);

	vwlan_free_old_tx(priv);
	if (netif_queue_stopped(priv->netdev) &&
	    priv->tx_vq->num_free >= MAX_SKB_FRAGS + 2)
		netif_wake_queue(priv->netdev);
}

static netdev_tx_t vwlan_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct virtio_wlan *priv = *(struct virtio_wlan **)netdev_priv(dev);
	struct scatterlist sg[MAX_SKB_FRAGS + 1];
	unsigned int tx_bytes = skb->len;
	int num_sg, err;

	guard(spinlock_irqsave)(&priv->tx_lock);

	vwlan_free_old_tx(priv);

	sg_init_table(sg, ARRAY_SIZE(sg));
	num_sg = skb_to_sgvec(skb, sg, 0, skb->len);
	if (num_sg < 0) {
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	err = virtqueue_add_outbuf(priv->tx_vq, sg, num_sg, skb, GFP_ATOMIC);
	if (err) {
		if (err == -ENOSPC) {
			netif_stop_queue(dev);
			/*
			 * Make sure the descriptors already queued are
			 * visible to the device: the wake depends on one
			 * of them completing.
			 */
			virtqueue_kick(priv->tx_vq);
			return NETDEV_TX_BUSY;
		}
		dev->stats.tx_dropped++;
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* The skb belongs to the device from here on. */
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += tx_bytes;

	/*
	 * Stop before deciding whether to notify, not after: the wake
	 * depends on a completion, which depends on the notification, so
	 * a batch that stops the queue must still kick it.
	 */
	if (priv->tx_vq->num_free < MAX_SKB_FRAGS + 2)
		netif_stop_queue(dev);

	if (!netdev_xmit_more() || netif_queue_stopped(dev))
		virtqueue_kick(priv->tx_vq);

	return NETDEV_TX_OK;
}

static void vwlan_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct virtio_wlan *priv = *(struct virtio_wlan **)netdev_priv(dev);

	netdev_warn(dev, "transmit timed out on queue %u\n", txqueue);

	/*
	 * Recover from a lost notification: reclaim anything the device
	 * finished, re-kick in case it never saw the last batch, and
	 * restart the queue if there is room again.
	 */
	guard(spinlock_irqsave)(&priv->tx_lock);

	vwlan_free_old_tx(priv);
	virtqueue_kick(priv->tx_vq);
	if (netif_queue_stopped(dev) &&
	    priv->tx_vq->num_free >= MAX_SKB_FRAGS + 2)
		netif_wake_queue(dev);
}

static int vwlan_open(struct net_device *dev)
{
	struct virtio_wlan *priv = *(struct virtio_wlan **)netdev_priv(dev);

	/*
	 * Fill while NAPI is still disabled: a poll running concurrently
	 * would touch the receive queue from softirq context while this
	 * one is adding to it, and nothing serialises the two.
	 */
	vwlan_fill_rx(priv, GFP_KERNEL);

	napi_enable(&priv->napi);
	vwlan_enable_refill(priv);

	/*
	 * Buffers the device completed during the fill raised a callback
	 * that could not schedule a disabled NAPI, and no further
	 * interrupt is coming for them; poll once to pick them up.
	 */
	local_bh_disable();
	napi_schedule(&priv->napi);
	local_bh_enable();

	netif_start_queue(dev);

	return 0;
}

static int vwlan_stop(struct net_device *dev)
{
	struct virtio_wlan *priv = *(struct virtio_wlan **)netdev_priv(dev);

	/* Stop the refill before NAPI: it parks NAPI itself. */
	vwlan_disable_refill(priv);
	cancel_delayed_work_sync(&priv->refill);

	netif_stop_queue(dev);
	napi_disable(&priv->napi);
	netif_carrier_off(dev);

	/*
	 * cfg80211 frees the scan request from its NETDEV_DOWN handler,
	 * which runs after ndo_stop(); complete it here so that the
	 * driver never retains a pointer cfg80211 has already freed.
	 */
	vwlan_scan_finish(priv, true);
	cancel_delayed_work_sync(&priv->scan_timeout);

	scoped_guard(mutex, &priv->state_lock) {
		priv->connecting = false;
		priv->connected = false;
	}

	return 0;
}

static const struct net_device_ops vwlan_netdev_ops = {
	.ndo_open		= vwlan_open,
	.ndo_stop		= vwlan_stop,
	.ndo_start_xmit		= vwlan_xmit,
	.ndo_tx_timeout		= vwlan_tx_timeout,
	.ndo_validate_addr	= eth_validate_addr,
};

/* cfg80211 operations */

static int vwlan_scan(struct wiphy *wiphy, struct cfg80211_scan_request *request)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_scan *cmd;
	size_t len, i;
	__le32 *freqs;
	u8 *p;
	int ret;

	if (request->n_ssids > VWLAN_MAX_SCAN_SSIDS)
		return -EINVAL;

	len = sizeof(*cmd) +
	      request->n_ssids * sizeof(struct virtio_wlan_ssid) +
	      request->n_channels * sizeof(__le32) + request->ie_len;
	if (sizeof(struct virtio_wlan_cmd_hdr) + len > VWLAN_CMD_BUF_SIZE)
		return -EINVAL;

	scoped_guard(mutex, &priv->state_lock)
		if (priv->scan_req)
			return -EBUSY;

	cmd = kzalloc(len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	cmd->n_ssids = cpu_to_le32(request->n_ssids);
	cmd->n_channels = cpu_to_le32(request->n_channels);
	cmd->ie_len = cpu_to_le32(request->ie_len);

	p = (u8 *)(cmd + 1);
	for (i = 0; i < request->n_ssids; i++) {
		struct virtio_wlan_ssid *ssid = (struct virtio_wlan_ssid *)p;

		ssid->ssid_len = min_t(u8, request->ssids[i].ssid_len,
				       VIRTIO_WLAN_MAX_SSID_LEN);
		memcpy(ssid->ssid, request->ssids[i].ssid, ssid->ssid_len);
		p += sizeof(*ssid);
	}

	freqs = (__le32 *)p;
	for (i = 0; i < request->n_channels; i++)
		freqs[i] = cpu_to_le32(request->channels[i]->center_freq);
	p += request->n_channels * sizeof(__le32);

	if (request->ie_len)
		memcpy(p, request->ie, request->ie_len);

	ret = vwlan_cmd(priv, VIRTIO_WLAN_CMD_SCAN, cmd, len, NULL, 0, NULL);
	kfree(cmd);

	/*
	 * Only adopt the request once the device has accepted it: on
	 * failure cfg80211 frees it as soon as this returns, so it must
	 * not be reachable from the event or timeout paths.
	 */
	if (ret)
		return ret;

	scoped_guard(mutex, &priv->state_lock) {
		priv->scan_req = request;
		priv->scan_started = jiffies;
	}

	/* A scan the device never finishes would wedge scanning. */
	schedule_delayed_work(&priv->scan_timeout, VWLAN_SCAN_TIMEOUT);

	return 0;
}

static void vwlan_abort_scan(struct wiphy *wiphy, struct wireless_dev *wdev)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);

	vwlan_cmd(priv, VIRTIO_WLAN_CMD_ABORT_SCAN, NULL, 0, NULL, 0, NULL);
}

static int vwlan_connect(struct wiphy *wiphy, struct net_device *dev,
			 struct cfg80211_connect_params *sme)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct ieee80211_channel *chan;
	struct virtio_wlan_cmd_connect *cmd;
	const u8 *bssid;
	size_t len;
	int ret, i;

	if (sme->ssid_len > VIRTIO_WLAN_MAX_SSID_LEN)
		return -EINVAL;
	if (sme->crypto.n_ciphers_pairwise > VIRTIO_WLAN_MAX_CIPHERS ||
	    sme->crypto.n_akm_suites > VIRTIO_WLAN_MAX_AKMS)
		return -EINVAL;

	len = sizeof(*cmd) + sme->ie_len;
	if (sizeof(struct virtio_wlan_cmd_hdr) + len > VWLAN_CMD_BUF_SIZE)
		return -EINVAL;

	cmd = kzalloc(len, GFP_KERNEL);
	if (!cmd)
		return -ENOMEM;

	bssid = sme->bssid ? sme->bssid : sme->bssid_hint;
	if (bssid) {
		memcpy(cmd->bssid, bssid, ETH_ALEN);
		cmd->bssid_valid = 1;
	}

	cmd->ssid_len = sme->ssid_len;
	memcpy(cmd->ssid, sme->ssid, sme->ssid_len);
	cmd->privacy = sme->privacy;
	cmd->want_1x = sme->want_1x;

	chan = sme->channel ? sme->channel : sme->channel_hint;
	if (chan)
		cmd->center_freq = cpu_to_le32(chan->center_freq);

	cmd->auth_type = cpu_to_le32(sme->auth_type);
	cmd->mfp = cpu_to_le32(sme->mfp);
	cmd->wpa_versions = cpu_to_le32(sme->crypto.wpa_versions);
	cmd->cipher_group = cpu_to_le32(sme->crypto.cipher_group);

	cmd->n_ciphers_pairwise = cpu_to_le32(sme->crypto.n_ciphers_pairwise);
	for (i = 0; i < sme->crypto.n_ciphers_pairwise; i++)
		cmd->ciphers_pairwise[i] =
			cpu_to_le32(sme->crypto.ciphers_pairwise[i]);

	cmd->n_akm_suites = cpu_to_le32(sme->crypto.n_akm_suites);
	for (i = 0; i < sme->crypto.n_akm_suites; i++)
		cmd->akm_suites[i] = cpu_to_le32(sme->crypto.akm_suites[i]);

	cmd->ie_len = cpu_to_le32(sme->ie_len);
	if (sme->ie_len)
		memcpy(cmd + 1, sme->ie, sme->ie_len);

	ret = vwlan_cmd(priv, VIRTIO_WLAN_CMD_CONNECT, cmd, len, NULL, 0, NULL);
	kfree(cmd);
	if (ret)
		return ret;

	/* Remember what was asked for: a result naming anything else is
	 * refused rather than passed to cfg80211. */
	scoped_guard(mutex, &priv->state_lock) {
		priv->connecting = true;
		priv->connect_ssid_len = sme->ssid_len;
		memcpy(priv->connect_ssid, sme->ssid, sme->ssid_len);
	}

	return 0;
}

static int vwlan_disconnect(struct wiphy *wiphy, struct net_device *dev,
			    u16 reason_code)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_disconnect cmd = {
		.reason_code = cpu_to_le16(reason_code),
	};
	int ret;

	ret = vwlan_cmd(priv, VIRTIO_WLAN_CMD_DISCONNECT, &cmd, sizeof(cmd),
			NULL, 0, NULL);

	scoped_guard(mutex, &priv->state_lock)
		priv->connecting = false;

	return ret;
}

static int vwlan_add_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			 int link_id, u8 key_index, bool pairwise,
			 const u8 *mac_addr, struct key_params *params)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_add_key cmd = {};

	if (params->key_len > VIRTIO_WLAN_MAX_KEY_LEN ||
	    params->seq_len > VIRTIO_WLAN_MAX_SEQ_LEN)
		return -EINVAL;

	cmd.key_idx = key_index;
	cmd.pairwise = pairwise;
	if (mac_addr) {
		memcpy(cmd.mac, mac_addr, ETH_ALEN);
		cmd.mac_valid = 1;
	}
	cmd.cipher = cpu_to_le32(params->cipher);
	cmd.key_len = params->key_len;
	if (params->key_len)
		memcpy(cmd.key, params->key, params->key_len);
	cmd.seq_len = params->seq_len;
	if (params->seq_len)
		memcpy(cmd.seq, params->seq, params->seq_len);

	return vwlan_cmd(priv, VIRTIO_WLAN_CMD_ADD_KEY, &cmd, sizeof(cmd),
			 NULL, 0, NULL);
}

static int vwlan_del_key(struct wiphy *wiphy, struct wireless_dev *wdev,
			 int link_id, u8 key_index, bool pairwise,
			 const u8 *mac_addr)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_del_key cmd = {};

	cmd.key_idx = key_index;
	cmd.pairwise = pairwise;
	if (mac_addr) {
		memcpy(cmd.mac, mac_addr, ETH_ALEN);
		cmd.mac_valid = 1;
	}

	return vwlan_cmd(priv, VIRTIO_WLAN_CMD_DEL_KEY, &cmd, sizeof(cmd),
			 NULL, 0, NULL);
}

static int vwlan_set_default_key(struct wiphy *wiphy, struct net_device *dev,
				 int link_id, u8 key_index, bool unicast,
				 bool multicast)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_set_default_key cmd = {
		.key_idx = key_index,
		.unicast = unicast,
		.multicast = multicast,
	};

	return vwlan_cmd(priv, VIRTIO_WLAN_CMD_SET_DEFAULT_KEY, &cmd,
			 sizeof(cmd), NULL, 0, NULL);
}

/* Installs the IGTK index used when management frame protection is on. */
static int vwlan_set_default_mgmt_key(struct wiphy *wiphy,
				      struct wireless_dev *wdev, int link_id,
				      u8 key_index)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_set_default_mgmt_key cmd = {
		.key_idx = key_index,
	};

	return vwlan_cmd(priv, VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY, &cmd,
			 sizeof(cmd), NULL, 0, NULL);
}

static int vwlan_get_station(struct wiphy *wiphy, struct wireless_dev *wdev,
			     const u8 *mac, struct station_info *sinfo)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_get_station cmd = {};
	struct virtio_wlan_station_info info;
	size_t resp_len;
	u32 filled;
	int ret;

	memcpy(cmd.mac, mac, ETH_ALEN);

	ret = vwlan_cmd(priv, VIRTIO_WLAN_CMD_GET_STATION, &cmd, sizeof(cmd),
			&info, sizeof(info), &resp_len);
	if (ret)
		return ret;
	if (resp_len < sizeof(info))
		return -EIO;

	filled = le32_to_cpu(info.filled);

	if (filled & VIRTIO_WLAN_STA_F_SIGNAL) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_SIGNAL);
		sinfo->signal = (s32)le32_to_cpu(info.signal);
	}
	if (filled & VIRTIO_WLAN_STA_F_TX_BITRATE) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_TX_BITRATE);
		sinfo->txrate.legacy = min_t(u32, le32_to_cpu(info.tx_bitrate),
					     U16_MAX);
	}
	if (filled & VIRTIO_WLAN_STA_F_RX_BITRATE) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_BITRATE);
		sinfo->rxrate.legacy = min_t(u32, le32_to_cpu(info.rx_bitrate),
					     U16_MAX);
	}
	if (filled & VIRTIO_WLAN_STA_F_COUNTERS) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_RX_PACKETS) |
				 BIT_ULL(NL80211_STA_INFO_TX_PACKETS) |
				 BIT_ULL(NL80211_STA_INFO_RX_BYTES64) |
				 BIT_ULL(NL80211_STA_INFO_TX_BYTES64) |
				 BIT_ULL(NL80211_STA_INFO_TX_FAILED);
		sinfo->rx_packets = le64_to_cpu(info.rx_packets);
		sinfo->tx_packets = le64_to_cpu(info.tx_packets);
		sinfo->rx_bytes = le64_to_cpu(info.rx_bytes);
		sinfo->tx_bytes = le64_to_cpu(info.tx_bytes);
		sinfo->tx_failed = le32_to_cpu(info.tx_failed);
	}
	if (filled & VIRTIO_WLAN_STA_F_CONNECTED_TIME) {
		sinfo->filled |= BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME);
		sinfo->connected_time = le32_to_cpu(info.connected_time);
	}

	return 0;
}

/*
 * The supplicant opens the controlled port here once the four-way
 * handshake succeeds; relay it, since the port itself lives on the
 * device side.
 */
static int vwlan_change_station(struct wiphy *wiphy, struct wireless_dev *wdev,
				const u8 *mac,
				struct station_parameters *params)
{
	struct virtio_wlan *priv = wiphy_priv(wiphy);
	struct virtio_wlan_cmd_set_port_authorized cmd = {};
	u32 mask = params->sta_flags_mask;

	if (!(mask & BIT(NL80211_STA_FLAG_AUTHORIZED)))
		return 0;	/* nothing we model */

	memcpy(cmd.mac, mac, ETH_ALEN);
	cmd.authorized =
		!!(params->sta_flags_set & BIT(NL80211_STA_FLAG_AUTHORIZED));

	return vwlan_cmd(priv, VIRTIO_WLAN_CMD_SET_PORT_AUTHORIZED, &cmd,
			 sizeof(cmd), NULL, 0, NULL);
}

static int vwlan_change_iface(struct wiphy *wiphy, struct net_device *dev,
			      enum nl80211_iftype type,
			      struct vif_params *params)
{
	if (type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;

	dev->ieee80211_ptr->iftype = type;
	return 0;
}

static const struct cfg80211_ops vwlan_cfg80211_ops = {
	.scan			= vwlan_scan,
	.abort_scan		= vwlan_abort_scan,
	.connect		= vwlan_connect,
	.disconnect		= vwlan_disconnect,
	.add_key		= vwlan_add_key,
	.del_key		= vwlan_del_key,
	.set_default_key	= vwlan_set_default_key,
	.set_default_mgmt_key	= vwlan_set_default_mgmt_key,
	.get_station		= vwlan_get_station,
	.change_station		= vwlan_change_station,
	.change_virtual_intf	= vwlan_change_iface,
};

/* Device description */

/*
 * Returns a negative value for bands this driver cannot describe to
 * cfg80211. 6 GHz is deliberately excluded: wiphy_register() rejects a
 * 6 GHz band that carries no HE capabilities, and none are negotiated
 * yet.
 */
static int vwlan_band_to_nl80211(u8 band)
{
	switch (band) {
	case VIRTIO_WLAN_BAND_2GHZ:
		return NL80211_BAND_2GHZ;
	case VIRTIO_WLAN_BAND_5GHZ:
		return NL80211_BAND_5GHZ;
	default:
		return -EINVAL;
	}
}

static u32 vwlan_chan_flags(u32 flags)
{
	u32 out = 0;

	if (flags & VIRTIO_WLAN_CHAN_F_DISABLED)
		out |= IEEE80211_CHAN_DISABLED;
	if (flags & VIRTIO_WLAN_CHAN_F_NO_IR)
		out |= IEEE80211_CHAN_NO_IR;
	if (flags & VIRTIO_WLAN_CHAN_F_RADAR)
		out |= IEEE80211_CHAN_RADAR;
	if (flags & VIRTIO_WLAN_CHAN_F_NO_HT40PLUS)
		out |= IEEE80211_CHAN_NO_HT40PLUS;
	if (flags & VIRTIO_WLAN_CHAN_F_NO_HT40MINUS)
		out |= IEEE80211_CHAN_NO_HT40MINUS;
	if (flags & VIRTIO_WLAN_CHAN_F_NO_80MHZ)
		out |= IEEE80211_CHAN_NO_80MHZ;
	if (flags & VIRTIO_WLAN_CHAN_F_NO_160MHZ)
		out |= IEEE80211_CHAN_NO_160MHZ;

	return out;
}

static int vwlan_setup_ciphers(struct virtio_wlan *priv, const __le32 *ciphers,
			       u32 n_ciphers)
{
	u32 i, j, n = 0;

	if (!n_ciphers)
		return 0;

	priv->cipher_suites = kcalloc(n_ciphers, sizeof(u32), GFP_KERNEL);
	if (!priv->cipher_suites)
		return -ENOMEM;

	/* Duplicates make wiphy_register() reject the whole wiphy. */
	for (i = 0; i < n_ciphers; i++) {
		u32 suite = le32_to_cpu(ciphers[i]);

		for (j = 0; j < n; j++)
			if (priv->cipher_suites[j] == suite)
				break;
		if (j == n)
			priv->cipher_suites[n++] = suite;
	}

	priv->wiphy->cipher_suites = priv->cipher_suites;
	priv->wiphy->n_cipher_suites = n;

	return 0;
}

/*
 * Build the wiphy's bands from the device's channel list. The device
 * is untrusted, so every count and offset is bounded before use, and
 * channels in bands this driver cannot describe are dropped rather
 * than passed on to cfg80211.
 */
static int vwlan_setup_wiphy(struct virtio_wlan *priv, const void *resp,
			     size_t len)
{
	const struct virtio_wlan_wiphy_info *info = resp;
	const struct virtio_wlan_channel *chans;
	int per_band[NUM_NL80211_BANDS] = {};
	int idx[NUM_NL80211_BANDS] = {};
	u32 n_channels, n_ciphers;
	const __le32 *ciphers;
	bool have_band = false;
	size_t need;
	int err;
	u32 i;

	if (len < sizeof(*info))
		return -EIO;

	n_ciphers = le32_to_cpu(info->n_cipher_suites);
	n_channels = le32_to_cpu(info->n_channels);
	if (n_ciphers > VWLAN_MAX_CIPHER_SUITES ||
	    n_channels > VWLAN_MAX_CHANNELS || !n_channels)
		return -EIO;

	need = sizeof(*info) + n_ciphers * sizeof(__le32) +
	       n_channels * sizeof(*chans);
	if (len < need)
		return -EIO;

	ciphers = (const __le32 *)(info + 1);
	chans = (const struct virtio_wlan_channel *)(ciphers + n_ciphers);

	for (i = 0; i < n_channels; i++) {
		int band = vwlan_band_to_nl80211(chans[i].band);

		if (band >= 0 &&
		    le32_to_cpu(chans[i].center_freq) <= VWLAN_MAX_FREQ_MHZ)
			per_band[band]++;
	}

	for (i = 0; i < NUM_NL80211_BANDS; i++) {
		struct ieee80211_supported_band *sband = &priv->bands[i];

		if (!per_band[i])
			continue;

		priv->channels[i] = kcalloc(per_band[i],
					    sizeof(struct ieee80211_channel),
					    GFP_KERNEL);
		if (!priv->channels[i])
			return -ENOMEM;

		sband->band = i;
		sband->channels = priv->channels[i];
		sband->n_channels = per_band[i];
		if (i == NL80211_BAND_2GHZ) {
			memcpy(priv->rates_2ghz, vwlan_rate_template,
			       sizeof(priv->rates_2ghz));
			sband->bitrates = priv->rates_2ghz;
			sband->n_bitrates = ARRAY_SIZE(priv->rates_2ghz);
		} else {
			memcpy(priv->rates_ofdm,
			       vwlan_rate_template + VWLAN_OFDM_OFFSET,
			       sizeof(priv->rates_ofdm));
			sband->bitrates = priv->rates_ofdm;
			sband->n_bitrates = ARRAY_SIZE(priv->rates_ofdm);
		}
		priv->wiphy->bands[i] = sband;
		have_band = true;
	}

	if (!have_band)
		return -EIO;

	for (i = 0; i < n_channels; i++) {
		int band = vwlan_band_to_nl80211(chans[i].band);
		struct ieee80211_channel *chan;
		u32 freq = le32_to_cpu(chans[i].center_freq);

		/* Must drop exactly what the counting pass dropped. */
		if (band < 0 || freq > VWLAN_MAX_FREQ_MHZ)
			continue;

		chan = &priv->channels[band][idx[band]++];
		chan->band = band;
		chan->center_freq = freq;
		chan->flags = vwlan_chan_flags(le32_to_cpu(chans[i].flags));
		chan->max_power = min_t(u32, le32_to_cpu(chans[i].max_power),
					100);
	}

	err = vwlan_setup_ciphers(priv, ciphers, n_ciphers);
	if (err)
		return err;

	/* At least one SSID, or userspace cannot run an active scan. */
	priv->wiphy->max_scan_ssids =
		clamp_t(u32, le32_to_cpu(info->max_scan_ssids), 1,
			VWLAN_MAX_SCAN_SSIDS);
	priv->wiphy->max_scan_ie_len =
		min_t(u32, le32_to_cpu(info->max_scan_ie_len),
		      VWLAN_CMD_BUF_SIZE / 2);
	priv->wiphy->max_num_pmkids =
		min_t(u32, le32_to_cpu(info->max_num_pmkids), 32);

	return 0;
}

/* Setup and teardown */

static int vwlan_init_vqs(struct virtio_wlan *priv)
{
	struct virtqueue_info vqs_info[VIRTIO_WLAN_VQ_MAX] = {
		[VIRTIO_WLAN_VQ_RX]	= { "rx", vwlan_rx_cb },
		[VIRTIO_WLAN_VQ_TX]	= { "tx", vwlan_tx_cb },
		[VIRTIO_WLAN_VQ_CMD]	= { "cmd", vwlan_cmd_cb },
		[VIRTIO_WLAN_VQ_EVENT]	= { "event", vwlan_event_cb },
	};
	struct virtqueue *vqs[VIRTIO_WLAN_VQ_MAX];
	int err;

	err = virtio_find_vqs(priv->vdev, VIRTIO_WLAN_VQ_MAX, vqs, vqs_info,
			      NULL);
	if (err)
		return err;

	priv->rx_vq = vqs[VIRTIO_WLAN_VQ_RX];
	priv->tx_vq = vqs[VIRTIO_WLAN_VQ_TX];
	priv->cmd_vq = vqs[VIRTIO_WLAN_VQ_CMD];
	priv->event_vq = vqs[VIRTIO_WLAN_VQ_EVENT];

	/*
	 * Queue sizes come from the device. A transmit queue that cannot
	 * hold one worst-case packet would stop for good the first time
	 * it filled, since the wake condition could never be met.
	 */
	if (virtqueue_get_vring_size(priv->tx_vq) < MAX_SKB_FRAGS + 2 ||
	    virtqueue_get_vring_size(priv->cmd_vq) < 2 ||
	    !virtqueue_get_vring_size(priv->rx_vq) ||
	    !virtqueue_get_vring_size(priv->event_vq)) {
		dev_err(&priv->vdev->dev, "device offers unusable queue sizes\n");
		priv->vdev->config->del_vqs(priv->vdev);
		return -EINVAL;
	}

	return 0;
}

/*
 * Reclaim every buffer the device still owns. del_vqs() frees the ring
 * without returning tokens, so this has to run first or the skbs and
 * their DMA mappings leak.
 */
static void vwlan_drain_vqs(struct virtio_wlan *priv)
{
	void *buf;

	while ((buf = virtqueue_detach_unused_buf(priv->rx_vq)))
		dev_kfree_skb(buf);
	while ((buf = virtqueue_detach_unused_buf(priv->tx_vq)))
		dev_kfree_skb(buf);
	while (virtqueue_detach_unused_buf(priv->event_vq))
		;
	while (virtqueue_detach_unused_buf(priv->cmd_vq))
		;
}

static void vwlan_free_bufs(struct virtio_wlan *priv)
{
	int i;

	for (i = 0; i < VWLAN_NUM_EVENT_BUFS; i++)
		kfree(priv->event_bufs[i]);
	kfree(priv->cmd_req);
	kfree(priv->cmd_resp);
}

static int vwlan_alloc_bufs(struct virtio_wlan *priv)
{
	int i;

	priv->cmd_req = kzalloc(VWLAN_CMD_BUF_SIZE, GFP_KERNEL);
	priv->cmd_resp = kzalloc(VWLAN_RESP_BUF_SIZE, GFP_KERNEL);
	if (!priv->cmd_req || !priv->cmd_resp)
		return -ENOMEM;

	for (i = 0; i < VWLAN_NUM_EVENT_BUFS; i++) {
		priv->event_bufs[i] = kzalloc(VWLAN_EVENT_BUF_SIZE, GFP_KERNEL);
		if (!priv->event_bufs[i])
			return -ENOMEM;
	}

	return 0;
}

/*
 * Stop every path that the device can drive. After this returns, no
 * callback, work item or command can reach the device or the objects
 * built during probe, so teardown needs no further synchronisation.
 */
static void vwlan_quiesce(struct virtio_wlan *priv)
{
	smp_store_release(&priv->ready, false);

	/*
	 * Break before reset: breaking makes the virtqueue operations
	 * that other contexts (transmit, NAPI, the event work) may be
	 * executing right now fail instead of touching a device that is
	 * about to go away.
	 */
	virtio_break_device(priv->vdev);
	virtio_reset_device(priv->vdev);

	cancel_work_sync(&priv->event_work);

	/*
	 * Disable rather than cancel: both of these re-arm themselves,
	 * and a plain cancel could return with a fresh timer queued by
	 * the very run it waited for.
	 */
	vwlan_disable_refill(priv);
	disable_delayed_work_sync(&priv->refill);
	disable_delayed_work_sync(&priv->scan_timeout);

	scoped_guard(mutex, &priv->cmd_lock)
		priv->broken = true;
}

static int vwlan_probe(struct virtio_device *vdev)
{
	struct virtio_wlan *priv;
	struct net_device *netdev;
	struct wiphy *wiphy;
	u8 mac[ETH_ALEN];
	size_t resp_len;
	void *resp;
	int i, err;

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		return -ENODEV;

	wiphy = wiphy_new(&vwlan_cfg80211_ops, sizeof(*priv));
	if (!wiphy)
		return -ENOMEM;

	priv = wiphy_priv(wiphy);
	priv->wiphy = wiphy;
	priv->vdev = vdev;
	vdev->priv = priv;

	mutex_init(&priv->cmd_lock);
	mutex_init(&priv->state_lock);
	init_completion(&priv->cmd_done);
	spin_lock_init(&priv->tx_lock);
	spin_lock_init(&priv->refill_lock);
	INIT_WORK(&priv->event_work, vwlan_event_work);
	INIT_DELAYED_WORK(&priv->scan_timeout, vwlan_scan_timeout);
	INIT_DELAYED_WORK(&priv->refill, vwlan_refill_work);

	err = vwlan_alloc_bufs(priv);
	if (err)
		goto err_free_bufs;

	err = vwlan_init_vqs(priv);
	if (err)
		goto err_free_bufs;

	/*
	 * From here the device may raise interrupts on any queue at any
	 * time. Only the command queue is usable until ->ready is
	 * published at the end of probe; the other callbacks return
	 * immediately.
	 */
	virtio_device_ready(vdev);

	resp = kzalloc(VWLAN_RESP_BUF_SIZE, GFP_KERNEL);
	if (!resp) {
		err = -ENOMEM;
		goto err_quiesce;
	}

	err = vwlan_cmd(priv, VIRTIO_WLAN_CMD_GET_WIPHY, NULL, 0, resp,
			VWLAN_RESP_BUF_SIZE, &resp_len);
	if (!err)
		err = vwlan_setup_wiphy(priv, resp, resp_len);
	kfree(resp);
	if (err) {
		dev_err(&vdev->dev, "cannot query device description: %d\n",
			err);
		goto err_quiesce;
	}

	if (virtio_has_feature(vdev, VIRTIO_WLAN_F_MAC))
		virtio_cread_bytes(vdev,
				   offsetof(struct virtio_wlan_config, mac),
				   mac, ETH_ALEN);
	else
		eth_random_addr(mac);

	if (!is_valid_ether_addr(mac)) {
		dev_err(&vdev->dev, "device reported an invalid MAC address\n");
		err = -EIO;
		goto err_quiesce;
	}

	set_wiphy_dev(wiphy, &vdev->dev);
	wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	wiphy->signal_type = CFG80211_SIGNAL_TYPE_MBM;
	wiphy->max_num_akm_suites = VIRTIO_WLAN_MAX_AKMS;
	memcpy(wiphy->perm_addr, mac, ETH_ALEN);

	err = wiphy_register(wiphy);
	if (err)
		goto err_quiesce;

	netdev = alloc_netdev(sizeof(struct virtio_wlan *), "wlan%d",
			      NET_NAME_ENUM, ether_setup);
	if (!netdev) {
		err = -ENOMEM;
		goto err_quiesce_wiphy;
	}

	*(struct virtio_wlan **)netdev_priv(netdev) = priv;
	priv->netdev = netdev;

	netdev->netdev_ops = &vwlan_netdev_ops;
	netdev->watchdog_timeo = 5 * HZ;
	netdev->needs_free_netdev = true;
	SET_NETDEV_DEV(netdev, &vdev->dev);
	eth_hw_addr_set(netdev, mac);

	priv->wdev.wiphy = wiphy;
	priv->wdev.iftype = NL80211_IFTYPE_STATION;
	priv->wdev.netdev = netdev;
	netdev->ieee80211_ptr = &priv->wdev;

	netif_napi_add(netdev, &priv->napi, vwlan_poll);

	/*
	 * Everything the data path touches now exists, so let the device
	 * in before the interface becomes visible: register_netdev() can
	 * be followed immediately by an ndo_open from userspace, and a
	 * completion arriving while the callbacks were still gated would
	 * be lost for good, since nothing re-raises a virtio
	 * notification. The event queue is still empty, so an event
	 * callback firing now finds nothing to do and cannot reach the
	 * wireless_dev that register_netdev() is about to initialise.
	 */
	smp_store_release(&priv->ready, true);

	err = register_netdev(netdev);
	if (err)
		goto err_quiesce_netdev;

	netif_carrier_off(netdev);

	/* Events may be accepted now that the wireless_dev is live. */
	for (i = 0; i < VWLAN_NUM_EVENT_BUFS; i++) {
		if (vwlan_post_event_buf(priv, priv->event_bufs[i], GFP_KERNEL))
			break;
		priv->n_event_bufs++;
	}
	if (!priv->n_event_bufs) {
		dev_err(&vdev->dev, "cannot post event buffers\n");
		err = -ENOSPC;
		goto err_quiesce_netdev_registered;
	}
	virtqueue_kick(priv->event_vq);

	/* Collect anything the device posted while the queue was empty. */
	schedule_work(&priv->event_work);

	dev_info(&vdev->dev, "registered %s\n", netdev->name);
	return 0;

err_quiesce_netdev_registered:
	vwlan_quiesce(priv);
	unregister_netdev(netdev);
	goto err_unregister_wiphy;
err_quiesce_netdev:
	vwlan_quiesce(priv);
	/* free_netdev() deletes the NAPI instance for us. */
	free_netdev(netdev);
	goto err_unregister_wiphy;
err_quiesce_wiphy:
	vwlan_quiesce(priv);
err_unregister_wiphy:
	wiphy_unregister(wiphy);
	goto err_free_wiphy_data;
err_quiesce:
	vwlan_quiesce(priv);
err_free_wiphy_data:
	for (i = 0; i < NUM_NL80211_BANDS; i++)
		kfree(priv->channels[i]);
	kfree(priv->cipher_suites);
	vwlan_drain_vqs(priv);
	vdev->config->del_vqs(vdev);
err_free_bufs:
	vwlan_free_bufs(priv);
	wiphy_free(wiphy);
	return err;
}

static void vwlan_remove(struct virtio_device *vdev)
{
	struct virtio_wlan *priv = vdev->priv;
	int i;

	/* Nothing below races the device or the event work after this. */
	vwlan_quiesce(priv);

	/*
	 * ndo_stop() runs from here and completes any outstanding scan
	 * before cfg80211's NETDEV_DOWN handler would free the request.
	 */
	unregister_netdev(priv->netdev);
	wiphy_unregister(priv->wiphy);

	vwlan_drain_vqs(priv);
	vdev->config->del_vqs(vdev);

	for (i = 0; i < NUM_NL80211_BANDS; i++)
		kfree(priv->channels[i]);
	kfree(priv->cipher_suites);
	vwlan_free_bufs(priv);
	wiphy_free(priv->wiphy);
}

#ifdef CONFIG_PM_SLEEP
/*
 * The virtqueues are handed back over suspend, so the device loses all
 * posted buffers and any association. Tear the data path down here and
 * rebuild it on resume; userspace reconnects from a carrier-down
 * interface as it would after any link loss.
 */
static int vwlan_freeze(struct virtio_device *vdev)
{
	struct virtio_wlan *priv = vdev->priv;
	bool running = netif_running(priv->netdev);

	netif_device_detach(priv->netdev);

	smp_store_release(&priv->ready, false);

	vwlan_disable_refill(priv);
	cancel_delayed_work_sync(&priv->refill);

	/* Completing the scan first stops the timeout re-arming itself. */
	vwlan_scan_finish(priv, true);
	cancel_delayed_work_sync(&priv->scan_timeout);

	if (running)
		napi_disable(&priv->napi);

	scoped_guard(mutex, &priv->cmd_lock)
		priv->broken = true;

	scoped_guard(mutex, &priv->state_lock) {
		priv->connecting = false;
		priv->connected = false;
	}
	netif_carrier_off(priv->netdev);

	virtio_reset_device(vdev);
	cancel_work_sync(&priv->event_work);

	vwlan_drain_vqs(priv);
	vdev->config->del_vqs(vdev);
	priv->n_event_bufs = 0;

	return 0;
}

static int vwlan_restore(struct virtio_device *vdev)
{
	struct virtio_wlan *priv = vdev->priv;
	int i, err;

	err = vwlan_init_vqs(priv);
	if (err)
		return err;

	virtio_device_ready(vdev);

	scoped_guard(mutex, &priv->cmd_lock)
		priv->broken = false;

	smp_store_release(&priv->ready, true);

	for (i = 0; i < VWLAN_NUM_EVENT_BUFS; i++) {
		if (vwlan_post_event_buf(priv, priv->event_bufs[i], GFP_KERNEL))
			break;
		priv->n_event_bufs++;
	}
	virtqueue_kick(priv->event_vq);
	schedule_work(&priv->event_work);

	if (netif_running(priv->netdev)) {
		vwlan_fill_rx(priv, GFP_KERNEL);
		napi_enable(&priv->napi);
		local_bh_disable();
		napi_schedule(&priv->napi);
		local_bh_enable();
		vwlan_enable_refill(priv);
	}

	netif_device_attach(priv->netdev);

	return 0;
}
#endif /* CONFIG_PM_SLEEP */

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_MAC80211_WLAN, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, id_table);

static unsigned int features[] = {
	VIRTIO_WLAN_F_MAC,
};

static struct virtio_driver virtio_wlan_driver = {
	.feature_table		= features,
	.feature_table_size	= ARRAY_SIZE(features),
	.driver.name		= KBUILD_MODNAME,
	.id_table		= id_table,
	.probe			= vwlan_probe,
	.remove			= vwlan_remove,
#ifdef CONFIG_PM_SLEEP
	.freeze			= vwlan_freeze,
	.restore		= vwlan_restore,
#endif
};

module_virtio_driver(virtio_wlan_driver);

MODULE_DESCRIPTION("Virtio WLAN driver");
MODULE_LICENSE("GPL");
