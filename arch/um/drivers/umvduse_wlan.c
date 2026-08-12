// SPDX-License-Identifier: GPL-2.0
/*
 * umvduse-wlan: export a WLAN device driven inside this UML instance to
 * the host as a virtio-wlan device.
 *
 * The command queue carries cfg80211 requests, which are replayed
 * against the local wiphy through the in-kernel consumer API; results
 * come back as events. The data queues carry 802.3 frames, stolen from
 * the local netdev on receive and handed to dev_queue_xmit on send.
 *
 * The consequence is that everything which parses over-the-air input --
 * the driver, the firmware interface and mac80211's frame handling --
 * stays on this side of the boundary, while the host runs its ordinary
 * wireless userspace against a wiphy of its own.
 */

#include <linux/atomic.h>
#include <linux/etherdevice.h>
#include <linux/ieee80211.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/cfg80211_ik.h>
#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_wlan.h>

#include <init.h>

#include "umvduse.h"

/*
 * Receive depth is what stands between a burst and a drop. The host
 * refills only from its NAPI poll, so a buffer posted here is not
 * replaced until the shim completes it, the host is notified, polls,
 * reposts and kicks - a full round trip across the domain boundary. The
 * ring has to cover the worst scheduling delay over that path: 256
 * entries is 7ms at 35k packets/s, which a loaded UML thread misses
 * routinely, and every miss is a dropped frame and a retransmit.
 *
 * Transmit needs less: the host stops its queue when the ring fills, so
 * running short costs latency rather than packets. Commands and events
 * are low-rate and sized for their own concurrency.
 */
#define UMVD_WLAN_RX_QUEUE_SIZE		1024
#define UMVD_WLAN_TX_QUEUE_SIZE		512
#define UMVD_WLAN_CMD_QUEUE_SIZE	32
#define UMVD_WLAN_EVENT_QUEUE_SIZE	64
#define UMVD_WLAN_EVENT_MAX	2048
#define UMVD_WLAN_FRAME_MAX	2048
#define UMVD_WLAN_RESP_MAX	4096
#define UMVD_WLAN_MAX_SCAN_BSS	32
/* Long enough for a firmware load; ~15 s total. */
#define UMVD_WLAN_ATTACH_TRIES		150
#define UMVD_WLAN_ATTACH_DELAY_MS	100

/* An event payload has to leave room for the header. */
#define UMVD_WLAN_PAYLOAD_MAX \
	(UMVD_WLAN_EVENT_MAX - sizeof(struct virtio_wlan_event_hdr))

static char *ifname;
module_param(ifname, charp, 0444);
MODULE_PARM_DESC(ifname, "wireless interface to export (default: the first)");
__uml_help(ifname,
"umvduse_wlan.ifname=<name>\n"
"    Name of the wireless interface inside this UML instance to export\n"
"    to the host. Defaults to the first station-mode interface found.\n\n"
);

/* A request parked until there is something to put in it. */
struct umvd_wlan_parked {
	struct list_head list;
	struct umvd_request *req;
};

struct umvd_wlan {
	struct umvd_dev *dev;
	struct cfg80211_ik *ik;
	struct net_device *netdev;

	spinlock_t lock;
	struct list_head event_reqs;
	struct list_head rx_reqs;
	bool running;
	bool rx_handler_set;

	/* Scan results are gathered in process context. */
	struct work_struct scan_work;
	atomic_t scan_pending;
	bool scan_aborted;

	/* Statistics; a device that misbehaves shows up here. */
	u64 events_dropped;
	u64 rx_dropped;
	u64 tx_errors;
	u64 rx_count;
	u64 tx_count;
};

static struct umvd_wlan *the_wlan;

/* kiov helpers */

static size_t kiov_to_buf(struct vringh_kiov *kiov, void *buf, size_t len)
{
	size_t off = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used && off < len; i++) {
		size_t c = min_t(size_t, kiov->iov[i].iov_len, len - off);

		memcpy((u8 *)buf + off, kiov->iov[i].iov_base, c);
		off += c;
	}
	return off;
}

static size_t kiov_from_buf(struct vringh_kiov *kiov, const void *buf,
			    size_t len)
{
	size_t off = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used && off < len; i++) {
		size_t c = min_t(size_t, kiov->iov[i].iov_len, len - off);

		memcpy(kiov->iov[i].iov_base, (const u8 *)buf + off, c);
		off += c;
	}
	return off;
}

static size_t kiov_len(struct vringh_kiov *kiov)
{
	size_t total = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++)
		total += kiov->iov[i].iov_len;
	return total;
}

/* Parked-request bookkeeping */

static int park_request(struct umvd_wlan *w, struct list_head *head,
			struct umvd_request *req)
{
	struct umvd_wlan_parked *p;

	p = kmalloc(sizeof(*p), GFP_ATOMIC);
	if (!p) {
		umvd_request_complete(req, 0);
		return -ENOMEM;
	}
	p->req = req;

	scoped_guard(spinlock_irqsave, &w->lock) {
		if (w->running) {
			list_add_tail(&p->list, head);
			return 0;
		}
	}

	/*
	 * Completing performs a host ioctl and takes the transport's own
	 * queue lock, so it must not happen under w->lock with interrupts
	 * disabled.
	 */
	kfree(p);
	umvd_request_complete(req, 0);
	return -ENODEV;
}

static struct umvd_request *take_request(struct umvd_wlan *w,
					 struct list_head *head)
{
	struct umvd_wlan_parked *p;
	struct umvd_request *req;

	guard(spinlock_irqsave)(&w->lock);

	p = list_first_entry_or_null(head, struct umvd_wlan_parked, list);
	if (!p)
		return NULL;
	list_del(&p->list);
	req = p->req;
	kfree(p);
	return req;
}

static void drain_parked(struct umvd_wlan *w, struct list_head *head)
{
	struct umvd_wlan_parked *p, *n;
	LIST_HEAD(dead);

	scoped_guard(spinlock_irqsave, &w->lock)
		list_splice_init(head, &dead);

	list_for_each_entry_safe(p, n, &dead, list) {
		list_del(&p->list);
		/* The transport discards completions for a dead ring. */
		umvd_request_complete(p->req, 0);
		kfree(p);
	}
}

/* Events */

static void wlan_emit_event(struct umvd_wlan *w, u16 type,
			    const void *payload, size_t len)
{
	struct virtio_wlan_event_hdr hdr;
	struct umvd_request *req;
	size_t written;

	if (sizeof(hdr) + len > UMVD_WLAN_EVENT_MAX) {
		w->events_dropped++;
		pr_warn_ratelimited("umvduse-wlan: event %u too large (%zu)\n",
				    type, len);
		return;
	}

	req = take_request(w, &w->event_reqs);
	if (!req) {
		w->events_dropped++;
		pr_warn_ratelimited("umvduse-wlan: no event buffer, dropped event %u (total %llu)\n",
				    type, w->events_dropped);
		return;
	}

	if (kiov_len(&req->wiov) < sizeof(hdr) + len) {
		/* hdr.len must never describe more than was written. */
		w->events_dropped++;
		umvd_request_complete(req, 0);
		return;
	}

	hdr.type = cpu_to_le16(type);
	hdr.len = cpu_to_le16((u16)len);
	hdr.reserved = 0;

	written = kiov_from_buf(&req->wiov, &hdr, sizeof(hdr));
	if (written == sizeof(hdr) && len) {
		/* The payload continues after the header; copy it by
		 * walking the same iov with an offset. */
		size_t off = 0;
		unsigned int i;
		size_t skip = sizeof(hdr);

		for (i = req->wiov.i; i < req->wiov.used && off < len; i++) {
			u8 *base = req->wiov.iov[i].iov_base;
			size_t avail = req->wiov.iov[i].iov_len;
			size_t c;

			if (skip >= avail) {
				skip -= avail;
				continue;
			}
			base += skip;
			avail -= skip;
			skip = 0;

			c = min_t(size_t, avail, len - off);
			memcpy(base, (const u8 *)payload + off, c);
			off += c;
		}
		written += off;
	}

	umvd_request_complete(req, written);
}

/*
 * A copy of what an event needs from one BSS. The iterator runs under
 * rdev->bss_lock, so it may only memcpy: it cannot allocate, take a
 * reference (cfg80211_ref_bss() takes that same lock) or make a host
 * call.
 */
#define UMVD_WLAN_SNAP_IE_MAX	1024

struct wlan_bss_snapshot {
	u8 bssid[ETH_ALEN];
	u16 capability;
	u16 beacon_interval;
	u32 freq;
	u32 signal;
	u64 tsf;
	u32 ie_len;
	u8 ie[UMVD_WLAN_SNAP_IE_MAX];
};

struct wlan_bss_collect {
	int n;
	struct wlan_bss_snapshot bss[UMVD_WLAN_MAX_SCAN_BSS];
};

static void wlan_collect_bss(struct wiphy *wiphy, struct cfg80211_bss *bss,
			     void *data)
{
	struct wlan_bss_collect *c = data;
	const struct cfg80211_bss_ies *ies;
	struct wlan_bss_snapshot *snap;

	if (c->n >= UMVD_WLAN_MAX_SCAN_BSS)
		return;

	snap = &c->bss[c->n];

	memcpy(snap->bssid, bss->bssid, ETH_ALEN);
	snap->capability = bss->capability;
	snap->beacon_interval = bss->beacon_interval;
	snap->freq = bss->channel ? bss->channel->center_freq : 0;
	snap->signal = (u32)bss->signal;

	rcu_read_lock();
	ies = rcu_dereference(bss->ies);
	if (ies) {
		snap->tsf = ies->tsf;
		snap->ie_len = min_t(u32, ies->len, UMVD_WLAN_SNAP_IE_MAX);
		memcpy(snap->ie, ies->data, snap->ie_len);
	}
	rcu_read_unlock();

	c->n++;
}

/* One scan result, formatted from a snapshot taken above. */
static void wlan_emit_bss(struct umvd_wlan *w,
			  const struct wlan_bss_snapshot *snap)
{
	struct virtio_wlan_ev_scan_result *r;
	size_t ie_len;
	u8 *buf;

	buf = kzalloc(UMVD_WLAN_EVENT_MAX, GFP_KERNEL);
	if (!buf)
		return;
	r = (void *)buf;

	ie_len = min_t(size_t, snap->ie_len,
		       UMVD_WLAN_PAYLOAD_MAX - sizeof(*r));
	memcpy(buf + sizeof(*r), snap->ie, ie_len);

	memcpy(r->bssid, snap->bssid, ETH_ALEN);
	r->ftype = VIRTIO_WLAN_FTYPE_UNKNOWN;
	r->tsf = cpu_to_le64(snap->tsf);
	r->capability = cpu_to_le16(snap->capability);
	r->beacon_interval = cpu_to_le16(snap->beacon_interval);
	r->center_freq = cpu_to_le32(snap->freq);
	r->signal = cpu_to_le32(snap->signal);
	r->ie_len = cpu_to_le32((u32)ie_len);

	wlan_emit_event(w, VIRTIO_WLAN_EV_SCAN_RESULT, buf,
			sizeof(*r) + ie_len);
	kfree(buf);
}

/*
 * Scan completion arrives in the driver's context, which may be atomic,
 * so results are gathered and reported from here. One pass per
 * completion: schedule_work() collapses, and merging two scans into one
 * report would leave the host's second scan outstanding for ever.
 */
static void wlan_scan_work(struct work_struct *work)
{
	struct umvd_wlan *w = container_of(work, struct umvd_wlan, scan_work);
	struct wlan_bss_collect *c;
	int i;

	while (atomic_dec_if_positive(&w->scan_pending) >= 0) {
		struct virtio_wlan_ev_scan_done done = {};
		bool aborted = READ_ONCE(w->scan_aborted);

		if (!aborted) {
			c = kvzalloc(sizeof(*c), GFP_KERNEL);
			if (c) {
				cfg80211_ik_bss_iter(w->ik, wlan_collect_bss, c);
				for (i = 0; i < c->n; i++)
					wlan_emit_bss(w, &c->bss[i]);
				pr_info("umvduse-wlan: reported %d BSS(es)\n",
					c->n);
				kvfree(c);
			}
		}

		done.aborted = aborted ? 1 : 0;
		wlan_emit_event(w, VIRTIO_WLAN_EV_SCAN_DONE, &done,
				sizeof(done));
		pr_info("umvduse-wlan: scan finished (aborted=%u)\n",
			done.aborted);
	}
}

static void wlan_cb_scan_done(void *priv, bool aborted)
{
	struct umvd_wlan *w = priv;

	pr_info("umvduse-wlan: scan_done callback (aborted=%d)\n", aborted);
	WRITE_ONCE(w->scan_aborted, aborted);
	atomic_inc(&w->scan_pending);
	schedule_work(&w->scan_work);
}

static void wlan_cb_connect_result(void *priv, const u8 *bssid, u16 status,
				   const u8 *req_ie, size_t req_ie_len,
				   const u8 *resp_ie, size_t resp_ie_len)
{
	struct umvd_wlan *w = priv;
	struct virtio_wlan_ev_connect_result *cr;
	size_t total;
	u8 *buf;

	if (sizeof(*cr) + req_ie_len + resp_ie_len > UMVD_WLAN_PAYLOAD_MAX) {
		req_ie_len = 0;
		resp_ie_len = 0;
	}

	buf = kzalloc(UMVD_WLAN_EVENT_MAX, GFP_ATOMIC);
	if (!buf)
		return;
	cr = (void *)buf;

	if (bssid)
		memcpy(cr->bssid, bssid, ETH_ALEN);
	cr->status = cpu_to_le16(status);
	cr->req_ie_len = cpu_to_le32((u32)req_ie_len);
	cr->resp_ie_len = cpu_to_le32((u32)resp_ie_len);

	total = sizeof(*cr);
	if (req_ie_len) {
		memcpy(buf + total, req_ie, req_ie_len);
		total += req_ie_len;
	}
	if (resp_ie_len) {
		memcpy(buf + total, resp_ie, resp_ie_len);
		total += resp_ie_len;
	}

	wlan_emit_event(w, VIRTIO_WLAN_EV_CONNECT_RESULT, buf, total);
	kfree(buf);
}

static void wlan_cb_disconnected(void *priv, u16 reason,
				 bool locally_generated,
				 const u8 *ie, size_t ie_len)
{
	struct umvd_wlan *w = priv;
	struct virtio_wlan_ev_disconnected *ev;
	u8 *buf;

	if (sizeof(*ev) + ie_len > UMVD_WLAN_PAYLOAD_MAX)
		ie_len = 0;

	buf = kzalloc(UMVD_WLAN_EVENT_MAX, GFP_ATOMIC);
	if (!buf)
		return;
	ev = (void *)buf;

	ev->reason = cpu_to_le16(reason);
	/* The wire field is from_ap, the opposite of locally_generated. */
	ev->from_ap = locally_generated ? 0 : 1;
	ev->ie_len = cpu_to_le32((u32)ie_len);
	if (ie_len)
		memcpy(buf + sizeof(*ev), ie, ie_len);

	wlan_emit_event(w, VIRTIO_WLAN_EV_DISCONNECTED, buf,
			sizeof(*ev) + ie_len);
	kfree(buf);
}

static const struct cfg80211_ik_ops wlan_ik_ops = {
	.scan_done	= wlan_cb_scan_done,
	.connect_result	= wlan_cb_connect_result,
	.disconnected	= wlan_cb_disconnected,
};

/* Commands */

static size_t wlan_cmd_get_wiphy(struct umvd_wlan *w, u8 *out, size_t cap)
{
	struct virtio_wlan_wiphy_info *info = (void *)out;
	struct wiphy *wiphy = cfg80211_ik_wiphy(w->ik);
	struct virtio_wlan_channel *chans;
	u32 n_chan = 0, n_ciphers, i;
	enum nl80211_band band;
	__le32 *ciphers;
	size_t need;

	if (!wiphy)
		return 0;

	n_ciphers = min_t(u32, wiphy->n_cipher_suites, 32);

	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		if (!wiphy->bands[band])
			continue;
		n_chan += wiphy->bands[band]->n_channels;
	}
	n_chan = min_t(u32, n_chan, 256);

	need = sizeof(*info) + n_ciphers * sizeof(__le32) +
	       n_chan * sizeof(*chans);
	if (need > cap || !n_chan)
		return 0;

	memset(out, 0, need);
	info->max_scan_ssids = cpu_to_le32(wiphy->max_scan_ssids);
	info->max_scan_ie_len = cpu_to_le32(wiphy->max_scan_ie_len);
	info->max_num_pmkids = cpu_to_le32(wiphy->max_num_pmkids);
	info->n_cipher_suites = cpu_to_le32(n_ciphers);
	info->n_channels = cpu_to_le32(n_chan);

	ciphers = (__le32 *)(info + 1);
	for (i = 0; i < n_ciphers; i++)
		ciphers[i] = cpu_to_le32(wiphy->cipher_suites[i]);

	chans = (struct virtio_wlan_channel *)(ciphers + n_ciphers);
	i = 0;
	for (band = 0; band < NUM_NL80211_BANDS; band++) {
		struct ieee80211_supported_band *sband = wiphy->bands[band];
		int j;
		u8 wire_band;

		if (!sband)
			continue;

		switch (band) {
		case NL80211_BAND_2GHZ:
			wire_band = VIRTIO_WLAN_BAND_2GHZ;
			break;
		case NL80211_BAND_5GHZ:
			wire_band = VIRTIO_WLAN_BAND_5GHZ;
			break;
		case NL80211_BAND_6GHZ:
			wire_band = VIRTIO_WLAN_BAND_6GHZ;
			break;
		default:
			continue;
		}

		for (j = 0; j < sband->n_channels && i < n_chan; j++) {
			struct ieee80211_channel *c = &sband->channels[j];
			u32 flags = 0;

			if (c->flags & IEEE80211_CHAN_DISABLED)
				flags |= VIRTIO_WLAN_CHAN_F_DISABLED;
			if (c->flags & IEEE80211_CHAN_NO_IR)
				flags |= VIRTIO_WLAN_CHAN_F_NO_IR;
			if (c->flags & IEEE80211_CHAN_RADAR)
				flags |= VIRTIO_WLAN_CHAN_F_RADAR;

			chans[i].center_freq = cpu_to_le32(c->center_freq);
			chans[i].flags = cpu_to_le32(flags);
			chans[i].max_power = cpu_to_le32(c->max_power);
			chans[i].band = wire_band;
			i++;
		}
	}

	info->n_channels = cpu_to_le32(i);
	return sizeof(*info) + n_ciphers * sizeof(__le32) +
	       i * sizeof(*chans);
}

static int wlan_cmd_scan(struct umvd_wlan *w, const u8 *p, size_t len)
{
	const struct virtio_wlan_cmd_scan *c = (const void *)p;
	struct cfg80211_ssid *ssids = NULL;
	u32 n_ssids, n_chan, ie_len;
	const u8 *ie = NULL;
	u32 *freqs = NULL;
	size_t need;
	int ret, i;

	if (len < sizeof(*c))
		return -EINVAL;

	n_ssids = le32_to_cpu(c->n_ssids);
	n_chan = le32_to_cpu(c->n_channels);
	ie_len = le32_to_cpu(c->ie_len);

	if (n_ssids > 16 || n_chan > 256 || ie_len > len)
		return -EINVAL;

	need = sizeof(*c) + n_ssids * sizeof(struct virtio_wlan_ssid) +
	       n_chan * sizeof(__le32) + ie_len;
	if (len < need)
		return -EINVAL;

	if (n_ssids) {
		const struct virtio_wlan_ssid *ws = (const void *)(c + 1);

		ssids = kcalloc(n_ssids, sizeof(*ssids), GFP_KERNEL);
		if (!ssids)
			return -ENOMEM;
		for (i = 0; i < (int)n_ssids; i++) {
			ssids[i].ssid_len = min_t(u8, ws[i].ssid_len,
						  IEEE80211_MAX_SSID_LEN);
			memcpy(ssids[i].ssid, ws[i].ssid, ssids[i].ssid_len);
		}
	}

	if (n_chan) {
		const __le32 *wf = (const void *)((const u8 *)(c + 1) +
			n_ssids * sizeof(struct virtio_wlan_ssid));

		freqs = kcalloc(n_chan, sizeof(*freqs), GFP_KERNEL);
		if (!freqs) {
			kfree(ssids);
			return -ENOMEM;
		}
		for (i = 0; i < (int)n_chan; i++)
			freqs[i] = le32_to_cpu(wf[i]);
	}

	if (ie_len)
		ie = (const u8 *)(c + 1) +
		     n_ssids * sizeof(struct virtio_wlan_ssid) +
		     n_chan * sizeof(__le32);

	ret = cfg80211_ik_scan(w->ik, ssids, n_ssids, freqs, n_chan,
			       ie, ie_len);

	kfree(ssids);
	kfree(freqs);
	return ret;
}

static int wlan_cmd_connect(struct umvd_wlan *w, const u8 *p, size_t len)
{
	const struct virtio_wlan_cmd_connect *c = (const void *)p;
	struct cfg80211_connect_params sme = {};
	struct wiphy *wiphy = cfg80211_ik_wiphy(w->ik);
	u32 n_ciphers, n_akms, ie_len, freq;
	int i;

	if (len < sizeof(*c))
		return -EINVAL;

	ie_len = le32_to_cpu(c->ie_len);
	if (ie_len > len || len < sizeof(*c) + ie_len)
		return -EINVAL;

	n_ciphers = le32_to_cpu(c->n_ciphers_pairwise);
	n_akms = le32_to_cpu(c->n_akm_suites);
	if (n_ciphers > VIRTIO_WLAN_MAX_CIPHERS ||
	    n_akms > VIRTIO_WLAN_MAX_AKMS ||
	    c->ssid_len > IEEE80211_MAX_SSID_LEN)
		return -EINVAL;

	sme.ssid = c->ssid;
	sme.ssid_len = c->ssid_len;
	if (c->bssid_valid)
		sme.bssid = c->bssid;

	freq = le32_to_cpu(c->center_freq);
	if (freq)
		sme.channel = ieee80211_get_channel(wiphy, freq);

	sme.auth_type = le32_to_cpu(c->auth_type);
	sme.mfp = le32_to_cpu(c->mfp);
	sme.privacy = !!c->privacy;
	sme.want_1x = !!c->want_1x;

	sme.crypto.wpa_versions = le32_to_cpu(c->wpa_versions);
	sme.crypto.cipher_group = le32_to_cpu(c->cipher_group);
	sme.crypto.n_ciphers_pairwise = n_ciphers;
	for (i = 0; i < (int)n_ciphers; i++)
		sme.crypto.ciphers_pairwise[i] =
			le32_to_cpu(c->ciphers_pairwise[i]);
	sme.crypto.n_akm_suites = n_akms;
	for (i = 0; i < (int)n_akms; i++)
		sme.crypto.akm_suites[i] = le32_to_cpu(c->akm_suites[i]);

	/* EAPOL crosses the data queues to the host's supplicant. */
	sme.crypto.control_port = true;
	sme.crypto.control_port_ethertype = cpu_to_be16(ETH_P_PAE);

	if (ie_len) {
		sme.ie = (const u8 *)(c + 1);
		sme.ie_len = ie_len;
	}

	return cfg80211_ik_connect(w->ik, &sme);
}

static int wlan_cmd_add_key(struct umvd_wlan *w, const u8 *p, size_t len)
{
	const struct virtio_wlan_cmd_add_key *c = (const void *)p;
	struct key_params params = {};

	if (len < sizeof(*c))
		return -EINVAL;
	if (c->key_len > VIRTIO_WLAN_MAX_KEY_LEN ||
	    c->seq_len > VIRTIO_WLAN_MAX_SEQ_LEN)
		return -EINVAL;

	params.cipher = le32_to_cpu(c->cipher);
	params.key = c->key;
	params.key_len = c->key_len;
	if (c->seq_len) {
		params.seq = c->seq;
		params.seq_len = c->seq_len;
	}

	return cfg80211_ik_add_key(w->ik, c->key_idx, !!c->pairwise,
				   c->mac_valid ? c->mac : NULL, &params);
}

static size_t wlan_cmd_get_station(struct umvd_wlan *w, const u8 *p,
				   size_t len, u8 *out, size_t cap,
				   int *err)
{
	const struct virtio_wlan_cmd_get_station *c = (const void *)p;
	struct virtio_wlan_station_info *si = (void *)out;
	struct station_info sinfo;
	int ret;

	if (len < sizeof(*c) || cap < sizeof(*si)) {
		*err = -EINVAL;
		return 0;
	}

	ret = cfg80211_ik_get_station(w->ik, c->mac, &sinfo);
	if (ret) {
		*err = ret;
		return 0;
	}

	memset(si, 0, sizeof(*si));
	if (sinfo.filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)) {
		si->filled |= cpu_to_le32(VIRTIO_WLAN_STA_F_SIGNAL);
		si->signal = cpu_to_le32((u32)sinfo.signal);
	}
	if (sinfo.filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE)) {
		si->filled |= cpu_to_le32(VIRTIO_WLAN_STA_F_TX_BITRATE);
		si->tx_bitrate = cpu_to_le32(sinfo.txrate.legacy);
	}
	if (sinfo.filled & BIT_ULL(NL80211_STA_INFO_RX_BITRATE)) {
		si->filled |= cpu_to_le32(VIRTIO_WLAN_STA_F_RX_BITRATE);
		si->rx_bitrate = cpu_to_le32(sinfo.rxrate.legacy);
	}
	si->filled |= cpu_to_le32(VIRTIO_WLAN_STA_F_COUNTERS);
	si->rx_packets = cpu_to_le64(sinfo.rx_packets);
	si->tx_packets = cpu_to_le64(sinfo.tx_packets);
	si->rx_bytes = cpu_to_le64(sinfo.rx_bytes);
	si->tx_bytes = cpu_to_le64(sinfo.tx_bytes);
	si->tx_failed = cpu_to_le32(sinfo.tx_failed);

	cfg80211_sinfo_release_content(&sinfo);

	*err = 0;
	return sizeof(*si);
}

static int errno_to_status(int err)
{
	switch (err) {
	case 0:
		return VIRTIO_WLAN_S_OK;
	case -EINVAL:
		return VIRTIO_WLAN_S_EINVAL;
	case -EOPNOTSUPP:
	case -ENOTSUPP:
		return VIRTIO_WLAN_S_ENOTSUPP;
	case -EBUSY:
		return VIRTIO_WLAN_S_EBUSY;
	case -ENOENT:
		return VIRTIO_WLAN_S_ENOENT;
	case -ENOMEM:
		return VIRTIO_WLAN_S_ENOMEM;
	default:
		return VIRTIO_WLAN_S_EIO;
	}
}

static void wlan_handle_cmd(struct umvd_wlan *w, struct umvd_request *req)
{
	struct virtio_wlan_cmd_resp *resp;
	struct virtio_wlan_cmd_hdr hdr;
	size_t reqlen, paylen, outlen = 0;
	u8 *reqbuf, *respbuf;
	int err = 0;
	u16 cmd;

	reqbuf = kzalloc(UMVD_WLAN_RESP_MAX, GFP_KERNEL);
	respbuf = kzalloc(UMVD_WLAN_RESP_MAX, GFP_KERNEL);
	if (!reqbuf || !respbuf) {
		kfree(reqbuf);
		kfree(respbuf);
		umvd_request_complete(req, 0);
		return;
	}

	reqlen = kiov_to_buf(&req->riov, reqbuf, UMVD_WLAN_RESP_MAX);
	if (reqlen < sizeof(hdr)) {
		err = -EINVAL;
		goto respond;
	}

	memcpy(&hdr, reqbuf, sizeof(hdr));
	cmd = le16_to_cpu(hdr.cmd);
	paylen = reqlen - sizeof(hdr);

	pr_debug("umvduse-wlan: cmd %u (%zu bytes)\n", cmd, paylen);

	switch (cmd) {
	case VIRTIO_WLAN_CMD_GET_WIPHY:
		outlen = wlan_cmd_get_wiphy(w, respbuf + sizeof(*resp),
					    UMVD_WLAN_RESP_MAX - sizeof(*resp));
		if (!outlen)
			err = -ENOMEM;
		break;
	case VIRTIO_WLAN_CMD_SCAN:
		err = wlan_cmd_scan(w, reqbuf + sizeof(hdr), paylen);
		pr_info("umvduse-wlan: scan requested -> %d\n", err);
		break;
	case VIRTIO_WLAN_CMD_ABORT_SCAN:
		err = cfg80211_ik_abort_scan(w->ik);
		break;
	case VIRTIO_WLAN_CMD_CONNECT:
		err = wlan_cmd_connect(w, reqbuf + sizeof(hdr), paylen);
		break;
	case VIRTIO_WLAN_CMD_DISCONNECT: {
		const struct virtio_wlan_cmd_disconnect *d =
			(const void *)(reqbuf + sizeof(hdr));
		u16 reason = WLAN_REASON_DEAUTH_LEAVING;

		if (paylen >= sizeof(*d))
			reason = le16_to_cpu(d->reason_code);
		err = cfg80211_ik_disconnect(w->ik, reason);
		break;
	}
	case VIRTIO_WLAN_CMD_ADD_KEY:
		err = wlan_cmd_add_key(w, reqbuf + sizeof(hdr), paylen);
		pr_info("umvduse-wlan: add_key -> %d\n", err);
		break;
	case VIRTIO_WLAN_CMD_DEL_KEY: {
		const struct virtio_wlan_cmd_del_key *d =
			(const void *)(reqbuf + sizeof(hdr));

		if (paylen < sizeof(*d)) {
			err = -EINVAL;
			break;
		}
		err = cfg80211_ik_del_key(w->ik, d->key_idx, !!d->pairwise,
					  d->mac_valid ? d->mac : NULL);
		break;
	}
	case VIRTIO_WLAN_CMD_SET_DEFAULT_KEY: {
		const struct virtio_wlan_cmd_set_default_key *d =
			(const void *)(reqbuf + sizeof(hdr));

		if (paylen < sizeof(*d)) {
			err = -EINVAL;
			break;
		}
		err = cfg80211_ik_set_default_key(w->ik, d->key_idx,
						  !!d->unicast, !!d->multicast);
		break;
	}
	case VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY: {
		const struct virtio_wlan_cmd_set_default_mgmt_key *d =
			(const void *)(reqbuf + sizeof(hdr));

		if (paylen < sizeof(*d)) {
			err = -EINVAL;
			break;
		}
		err = cfg80211_ik_set_default_mgmt_key(w->ik, d->key_idx);
		break;
	}
	case VIRTIO_WLAN_CMD_SET_PORT_AUTHORIZED: {
		const struct virtio_wlan_cmd_set_port_authorized *a =
			(const void *)(reqbuf + sizeof(hdr));

		if (paylen < sizeof(*a)) {
			err = -EINVAL;
			break;
		}
		err = cfg80211_ik_set_authorized(w->ik, a->mac,
						 !!a->authorized);
		pr_info("umvduse-wlan: port %s for %pM -> %d\n",
			a->authorized ? "authorized" : "closed", a->mac, err);
		break;
	}
	case VIRTIO_WLAN_CMD_GET_STATION:
		outlen = wlan_cmd_get_station(w, reqbuf + sizeof(hdr), paylen,
					      respbuf + sizeof(*resp),
					      UMVD_WLAN_RESP_MAX - sizeof(*resp),
					      &err);
		break;
	default:
		err = -EOPNOTSUPP;
		break;
	}

respond:
	resp = (void *)respbuf;
	resp->status = cpu_to_le16(errno_to_status(err));
	resp->reserved = 0;
	resp->reserved2 = 0;
	if (err)
		outlen = 0;

	umvd_request_complete(req,
			      kiov_from_buf(&req->wiov, respbuf,
					    sizeof(*resp) + outlen));

	kfree(reqbuf);
	kfree(respbuf);
}

/* Data path */

static void wlan_handle_tx(struct umvd_wlan *w, struct umvd_request *req)
{
	size_t len = kiov_len(&req->riov);
	struct sk_buff *skb;
	int ret;

	if (len < ETH_HLEN || len > UMVD_WLAN_FRAME_MAX) {
		w->tx_errors++;
		umvd_request_complete(req, 0);
		return;
	}

	skb = netdev_alloc_skb(w->netdev, len + NET_IP_ALIGN);
	if (!skb) {
		w->tx_errors++;
		umvd_request_complete(req, 0);
		return;
	}
	skb_reserve(skb, NET_IP_ALIGN);
	kiov_to_buf(&req->riov, skb_put(skb, len), len);

	skb->dev = w->netdev;
	skb->protocol = eth_type_trans(skb, w->netdev);
	skb_reset_network_header(skb);
	/* eth_type_trans pulled the header; put it back for transmit. */
	skb_push(skb, ETH_HLEN);

	if (++w->tx_count <= 10 || skb->protocol == htons(ETH_P_PAE))
		pr_info("umvduse-wlan: tx %llu proto 0x%04x %zu bytes%s\n",
			w->tx_count, ntohs(skb->protocol), len,
			skb->protocol == htons(ETH_P_PAE) ? " [EAPOL]" : "");

	ret = dev_queue_xmit(skb);
	if (ret != NET_XMIT_SUCCESS) {
		w->tx_errors++;
		pr_info_ratelimited("umvduse-wlan: tx returned %d (%llu errors)\n",
				    ret, w->tx_errors);
	}

	umvd_request_complete(req, 0);
}

/* Steal frames arriving on the local interface and hand them to the host. */
static rx_handler_result_t wlan_rx_handler(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct umvd_wlan *w = rcu_dereference(skb->dev->rx_handler_data);
	struct umvd_request *req;
	size_t len;

	if (!w || !w->running)
		return RX_HANDLER_PASS;

	/* Frames arrive with the ethernet header pulled. */
	skb_push(skb, ETH_HLEN);
	len = skb->len;

	if (len > UMVD_WLAN_FRAME_MAX) {
		w->rx_dropped++;
		pr_warn_ratelimited("umvduse-wlan: rx frame too large: %zu bytes (%llu dropped) -- is GRO coalescing?\n",
				    len, w->rx_dropped);
		goto drop;
	}

	/*
	 * Received frames are commonly non-linear: the driver puts the
	 * payload in page fragments and only the headers live in
	 * skb->data. Copying skb->len bytes from there would send the
	 * right length of the wrong bytes for anything that does not fit
	 * in the linear area.
	 */
	if (skb_linearize(skb)) {
		w->rx_dropped++;
		pr_warn_ratelimited("umvduse-wlan: cannot linearise %zu-byte frame (%llu dropped)\n",
				    len, w->rx_dropped);
		goto drop;
	}

	req = take_request(w, &w->rx_reqs);
	if (!req) {
		w->rx_dropped++;
		pr_info_ratelimited("umvduse-wlan: rx drop, no host buffer (proto 0x%04x, %zu bytes, %llu dropped)\n",
				    ntohs(skb->protocol), len, w->rx_dropped);
		goto drop;
	}

	if (++w->rx_count <= 10 || skb->protocol == htons(ETH_P_PAE))
		pr_info("umvduse-wlan: rx %llu proto 0x%04x %zu bytes%s\n",
			w->rx_count, ntohs(skb->protocol), len,
			skb->protocol == htons(ETH_P_PAE) ? " [EAPOL]" : "");

	umvd_request_complete(req, kiov_from_buf(&req->wiov, skb->data, len));

drop:
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

/* Shim entry points */

static void wlan_handle(struct umvd_dev *dev, struct umvd_request *req)
{
	struct umvd_wlan *w = dev->priv;
	u32 q = umvd_request_queue(req);

	switch (q) {
	case VIRTIO_WLAN_VQ_RX:
		park_request(w, &w->rx_reqs, req);
		break;
	case VIRTIO_WLAN_VQ_TX:
		wlan_handle_tx(w, req);
		break;
	case VIRTIO_WLAN_VQ_CMD:
		wlan_handle_cmd(w, req);
		break;
	case VIRTIO_WLAN_VQ_EVENT:
		park_request(w, &w->event_reqs, req);
		break;
	default:
		umvd_request_complete(req, 0);
		break;
	}
}

static int wlan_setup(struct umvd_dev *dev)
{
	struct umvd_wlan *w = dev->priv;
	struct virtio_wlan_config *cfg;
	struct cfg80211_ik *ik;
	int i;

	wait_for_device_probe();

	/*
	 * A real driver registers its netdev asynchronously, after its
	 * firmware has loaded, which can be well after this runs. Retry
	 * rather than give up: setup() is allowed to sleep, and there is
	 * no second chance once bring-up fails.
	 */
	for (i = 0; i < UMVD_WLAN_ATTACH_TRIES; i++) {
		ik = cfg80211_ik_attach(ifname, &wlan_ik_ops, w);
		if (!IS_ERR(ik) || PTR_ERR(ik) != -EPROBE_DEFER)
			break;
		msleep(UMVD_WLAN_ATTACH_DELAY_MS);
	}
	if (IS_ERR(ik)) {
		pr_err("umvduse-wlan: no wireless interface to export (%ld)\n",
		       PTR_ERR(ik));
		return PTR_ERR(ik);
	}
	w->ik = ik;
	w->netdev = cfg80211_ik_netdev(ik);

	dev->device_id = VIRTIO_ID_MAC80211_WLAN;
	dev->vendor_id = 0x1af4;
	dev->features = BIT_ULL(VIRTIO_WLAN_F_MAC);
	dev->num_queues = VIRTIO_WLAN_VQ_MAX;
	dev->queue_max[VIRTIO_WLAN_VQ_RX] = UMVD_WLAN_RX_QUEUE_SIZE;
	dev->queue_max[VIRTIO_WLAN_VQ_TX] = UMVD_WLAN_TX_QUEUE_SIZE;
	dev->queue_max[VIRTIO_WLAN_VQ_CMD] = UMVD_WLAN_CMD_QUEUE_SIZE;
	dev->queue_max[VIRTIO_WLAN_VQ_EVENT] = UMVD_WLAN_EVENT_QUEUE_SIZE;

	cfg = (void *)dev->config;
	memcpy(cfg->mac, w->netdev->dev_addr, ETH_ALEN);
	dev->config_size = sizeof(*cfg);

	pr_info("umvduse-wlan: exporting %s (%pM)\n",
		w->netdev->name, w->netdev->dev_addr);
	return 0;
}

static int wlan_start(struct umvd_dev *dev, u64 features)
{
	struct umvd_wlan *w = dev->priv;
	int err;

	/*
	 * Nothing else will: this UML has no userspace, so the interface
	 * is still down and mac80211 has no active vif. A scan submitted
	 * against a down interface never completes.
	 */
	rtnl_lock();
	err = dev_open(w->netdev, NULL);
	if (!err) {
		/*
		 * Receive offloads hand up coalesced super-frames far
		 * larger than an MTU-sized buffer. The host does its own
		 * reassembly, so this side must carry plain frames.
		 */
		w->netdev->wanted_features &= ~(NETIF_F_GRO | NETIF_F_LRO);
		netdev_update_features(w->netdev);
		dev_disable_lro(w->netdev);

		err = netdev_rx_handler_register(w->netdev, wlan_rx_handler, w);
	}
	rtnl_unlock();
	if (err) {
		pr_err("umvduse-wlan: cannot bring up %s: %d\n",
		       w->netdev->name, err);
		return err;
	}
	pr_info("umvduse-wlan: %s is up, exporting\n", w->netdev->name);
	w->rx_handler_set = true;

	scoped_guard(spinlock_irqsave, &w->lock)
		w->running = true;

	return 0;
}

static void wlan_reset(struct umvd_dev *dev)
{
	struct umvd_wlan *w = dev->priv;

	scoped_guard(spinlock_irqsave, &w->lock)
		w->running = false;

	/*
	 * Leave no session for the next host to inherit: it would see a
	 * fresh virtio device but a wiphy still associated, with the
	 * previous keys installed.
	 */
	if (w->ik)
		cfg80211_ik_disconnect(w->ik, WLAN_REASON_DEAUTH_LEAVING);

	if (w->rx_handler_set) {
		rtnl_lock();
		netdev_rx_handler_unregister(w->netdev);
		rtnl_unlock();
		w->rx_handler_set = false;
	}

	cancel_work_sync(&w->scan_work);

	/* Hand every parked buffer back; the transport discards them. */
	drain_parked(w, &w->rx_reqs);
	drain_parked(w, &w->event_reqs);
}

static void wlan_remove(struct umvd_dev *dev)
{
	struct umvd_wlan *w = dev->priv;

	wlan_reset(dev);
	cfg80211_ik_detach(w->ik);
	w->ik = NULL;
}

static const struct umvd_device_ops wlan_ops = {
	.name	= "wlan",
	.setup	= wlan_setup,
	.start	= wlan_start,
	.handle	= wlan_handle,
	.reset	= wlan_reset,
	.remove	= wlan_remove,
};

static int __init umvduse_wlan_init(void)
{
	struct umvd_wlan *w;
	int err;

	w = kzalloc(sizeof(*w), GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	spin_lock_init(&w->lock);
	INIT_LIST_HEAD(&w->event_reqs);
	INIT_LIST_HEAD(&w->rx_reqs);
	INIT_WORK(&w->scan_work, wlan_scan_work);
	atomic_set(&w->scan_pending, 0);

	err = umvd_register_device(&wlan_ops, w);
	if (err) {
		kfree(w);
		return err;
	}

	the_wlan = w;
	return 0;
}
late_initcall(umvduse_wlan_init);
