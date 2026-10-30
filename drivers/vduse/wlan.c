// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-guest-wlan: " fmt

#include <linux/completion.h>
#include <linux/ctype.h>
#include <linux/device/driver.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/jiffies.h>
#include <linux/kstrtox.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/seq_file.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/vduse_guest.h>
#include <linux/virtio_wlan.h>
#include <linux/workqueue.h>
#include <net/cfg80211.h>
#include <net/netdev_lock.h>
#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_net.h>

#define VDUSE_GUEST_WLAN_EVENT_SIZE 4096
#define VDUSE_GUEST_WLAN_WIPHY_SIZE 16384
#define VDUSE_GUEST_WLAN_BACKLOG 512
#define VDUSE_GUEST_WLAN_INLINE_IOVS 4
#define VDUSE_GUEST_WLAN_TIMEOUT 30
#define VDUSE_GUEST_WLAN_TX_LIMIT 256
#define VDUSE_GUEST_WLAN_RETRY_DELAY msecs_to_jiffies(10)

static const u16 vduse_guest_wlan_queue_sizes[VIRTIO_WLAN_VQ_MAX] = {
	[VIRTIO_WLAN_VQ_RX] = 1024,
	[VIRTIO_WLAN_VQ_TX] = 512,
	[VIRTIO_WLAN_VQ_CMD] = 32,
	[VIRTIO_WLAN_VQ_EVENT] = 128,
};

/*
 * Invalid_commands counts the commands answered with INVALID, by the backend
 * or by cfg80211. Bss_dropped counts the scan results that did not fit in an
 * event or in the backlog. Events_dropped counts the events that did not fit
 * in a buffer that the host posted.
 *
 * Tx_invalid counts the host's frames with a bad length or header, and
 * tx_spoofed those with another source address. Tx_dropped counts the frames
 * that found no memory or no interface. Rx_no_buffer counts the received
 * frames that found no buffer from the host, and rx_oversized those larger
 * than the MTU allows. Rx_bad_buffers counts the host's buffers that are too
 * small for a frame or cannot be written. Tx_chained counts the control port
 * frames whose destructor completed their chain.
 */
struct vduse_guest_wlan_stats {
	atomic64_t commands;
	atomic64_t invalid_commands;
	atomic64_t events;
	atomic64_t bss_dropped;
	atomic64_t events_dropped;
	atomic64_t tx_frames;
	atomic64_t tx_invalid;
	atomic64_t tx_spoofed;
	atomic64_t tx_dropped;
	atomic64_t tx_chained;
	atomic64_t rx_frames;
	atomic64_t rx_no_buffer;
	atomic64_t rx_oversized;
	atomic64_t rx_bad_buffers;
};

/* A control port frame's chain on the tx queue, completed by its destructor. */
struct vduse_guest_wlan_tx_chain {
	struct vduse_guest_wlan *wlan;
	u16 head;
	u32 gen;
};

/* A buffer that the host posted on the rx queue, waiting for a frame. */
struct vduse_guest_wlan_rx_buf {
	struct list_head list;
	u16 head;
	struct vringh_kiov wiov;
	struct kvec kvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
};

/* An event, with its header, waiting for a buffer on the event queue. */
struct vduse_guest_wlan_event {
	struct list_head list;
	u32 len;
	u8 data[];
};

enum vduse_guest_wlan_note_type {
	VDUSE_GUEST_WLAN_NOTE_SCAN_DONE,
	VDUSE_GUEST_WLAN_NOTE_CONNECT,
	VDUSE_GUEST_WLAN_NOTE_DISCONNECTED,
	VDUSE_GUEST_WLAN_NOTE_EVENT,
};

/*
 * A notification from cfg80211, with the id of the request that it ends,
 * which the note work turns into events. A note of type EVENT carries an
 * event that is already built.
 */
struct vduse_guest_wlan_note {
	struct list_head list;
	enum vduse_guest_wlan_note_type type;
	u32 id;
	bool aborted;
	int status;
	enum nl80211_timeout_reason timeout_reason;
	struct cfg80211_bss *bss;
	struct cfg80211_bss **scan_bss;
	int n_scan_bss;
	u8 *req_ie, *resp_ie, *ie;
	size_t req_ie_len, resp_ie_len, ie_len;
	u16 reason;
	bool locally_generated;
	struct vduse_guest_wlan_event *event;
};

struct vduse_guest_wlan {
	struct vduse_guest_dev *dev;
	struct net_device *netdev;
	struct wiphy *wiphy;
	struct cfg80211_client *client;
	struct virtio_wlan_config config;
	void *wiphy_resp;
	size_t wiphy_resp_len;
	/* The largest command that the wiphy's limits allow. */
	size_t cmd_size;

	/*
	 * Commands, notes and events are handled in order on one ordered
	 * workqueue, so a command's response is written before any event
	 * that reports its outcome.
	 */
	struct workqueue_struct *wq;
	struct work_struct cmd_work;
	struct work_struct note_work;
	struct work_struct event_work;
	struct work_struct country_work;

	/* Protects running, the request state, the notes and the backlog. */
	spinlock_t lock;
	bool running;
	bool scan_active;
	u32 scan_id;
	bool conn_active;
	u32 conn_id;
	struct list_head notes;
	struct list_head backlog;
	unsigned int backlog_len;

	/* Set under the RTNL. */
	bool gone;
	bool rx_registered;

	/*
	 * Kicks queue the data works on the CPU that handles them. Data_lock
	 * protects data_running, tx_drain and the rx buffers. Once the
	 * interface is gone, tx_drain makes the tx work complete the host's
	 * frames unsent until the device stops. The core kicks only a queue
	 * that the driver made ready, so tx_kicked, set by the first kick of
	 * the tx queue, tells that the drain can start before the next kick.
	 * Tx_pending counts the frames that the guest's stack still holds. At
	 * the limit the tx work sets tx_wake and stops, and the destructor
	 * that next runs clears it and queues the work again. Control_port is
	 * the connection's control port ethertype, and tx_gen, under
	 * data_lock, changes when the device stops, so that a control port
	 * frame of an earlier start does not complete its chain.
	 */
	struct delayed_work rx_work;
	struct work_struct tx_work;
	spinlock_t data_lock;
	bool data_running;
	bool tx_drain;
	struct list_head rx_bufs;
	atomic_t tx_pending;
	unsigned long tx_wake;
	unsigned long tx_kicked;
	__be16 control_port;
	u32 tx_gen;

	struct vduse_guest_wlan_stats stats;
};

static struct vduse_guest_wlan_event *
vduse_guest_wlan_event_alloc(u16 type, size_t len, gfp_t gfp)
{
	struct vduse_guest_wlan_event *ev;
	struct virtio_wlan_event_hdr *hdr;

	ev = kzalloc(struct_size(ev, data, sizeof(*hdr) + len), gfp);
	if (!ev)
		return NULL;

	ev->len = sizeof(*hdr) + len;
	hdr = (void *)ev->data;
	hdr->type = cpu_to_le16(type);
	hdr->len = cpu_to_le32(len);
	return ev;
}

static void *vduse_guest_wlan_event_payload(struct vduse_guest_wlan_event *ev)
{
	return ev->data + sizeof(struct virtio_wlan_event_hdr);
}

static u16 vduse_guest_wlan_event_type(struct vduse_guest_wlan_event *ev)
{
	return le16_to_cpu(((struct virtio_wlan_event_hdr *)ev->data)->type);
}

/*
 * BSS events are dropped once the backlog is full, and others are kept. A
 * COUNTRY event replaces one that is still in the backlog. Only the country
 * work queues them, on the same ordered workqueue as the event work, so the
 * event work never holds the one that is replaced.
 */
static void vduse_guest_wlan_queue_event(struct vduse_guest_wlan *wlan,
					 struct vduse_guest_wlan_event *ev,
					 bool droppable)
{
	struct vduse_guest_wlan_event *old = NULL, *pos;

	spin_lock_irq(&wlan->lock);
	if (!wlan->running ||
	    (droppable && wlan->backlog_len >= VDUSE_GUEST_WLAN_BACKLOG)) {
		spin_unlock_irq(&wlan->lock);
		if (droppable)
			atomic64_inc(&wlan->stats.bss_dropped);
		kfree(ev);
		return;
	}
	if (vduse_guest_wlan_event_type(ev) == VIRTIO_WLAN_EVENT_COUNTRY) {
		list_for_each_entry(pos, &wlan->backlog, list) {
			if (vduse_guest_wlan_event_type(pos) ==
			    VIRTIO_WLAN_EVENT_COUNTRY) {
				old = pos;
				break;
			}
		}
	}
	if (old) {
		list_replace(&old->list, &ev->list);
	} else {
		list_add_tail(&ev->list, &wlan->backlog);
		wlan->backlog_len++;
	}
	spin_unlock_irq(&wlan->lock);
	kfree(old);
}

static void vduse_guest_wlan_free_note(struct vduse_guest_wlan *wlan,
				       struct vduse_guest_wlan_note *note)
{
	int i;

	if (note->bss)
		cfg80211_put_bss(wlan->wiphy, note->bss);
	for (i = 0; i < note->n_scan_bss; i++)
		cfg80211_put_bss(wlan->wiphy, note->scan_bss[i]);
	kfree(note->scan_bss);
	kfree(note->req_ie);
	kfree(note->resp_ie);
	kfree(note->ie);
	kfree(note->event);
	kfree(note);
}

/* Add a note, unless the device is stopped, and have the note work run. */
static void vduse_guest_wlan_add_note(struct vduse_guest_wlan *wlan,
				      struct vduse_guest_wlan_note *note)
{
	unsigned long flags;

	spin_lock_irqsave(&wlan->lock, flags);
	if (!wlan->running) {
		spin_unlock_irqrestore(&wlan->lock, flags);
		vduse_guest_wlan_free_note(wlan, note);
		return;
	}
	list_add_tail(&note->list, &wlan->notes);
	spin_unlock_irqrestore(&wlan->lock, flags);

	queue_work(wlan->wq, &wlan->note_work);
}

static void vduse_guest_wlan_bss_info(struct cfg80211_bss *bss,
				      const struct cfg80211_bss_ies *ies,
				      struct virtio_wlan_bss_info *info,
				      size_t ie_len)
{
	info->tsf = cpu_to_le64(ies->tsf);
	info->signal = cpu_to_le32(bss->signal);
	info->freq = cpu_to_le16(bss->channel->center_freq);
	info->capability = cpu_to_le16(bss->capability);
	info->beacon_interval = cpu_to_le16(bss->beacon_interval);
	ether_addr_copy(info->bssid, bss->bssid);
	info->frame_type = ies->from_beacon ? VIRTIO_WLAN_FRAME_BEACON :
					      VIRTIO_WLAN_FRAME_PROBE_RESP;
	info->ie_len = cpu_to_le16(ie_len);
}

/*
 * Build a BSS event. The IEs pass through the allowlist, and an entry that
 * does not fit in one event is dropped.
 */
static void vduse_guest_wlan_report_bss(struct vduse_guest_wlan *wlan, u32 id,
					struct cfg80211_bss *bss)
{
	size_t room = VDUSE_GUEST_WLAN_EVENT_SIZE -
		      sizeof(struct virtio_wlan_event_hdr) -
		      sizeof(struct virtio_wlan_event_bss);
	const struct cfg80211_bss_ies *ies;
	struct vduse_guest_wlan_event *ev;
	struct virtio_wlan_event_bss *e;
	bool truncated;
	size_t n;
	u8 *buf;

	buf = kmalloc(room, GFP_KERNEL);
	if (!buf) {
		atomic64_inc(&wlan->stats.bss_dropped);
		return;
	}

	rcu_read_lock();
	ies = rcu_dereference(bss->ies);
	n = virtio_wlan_filter_elems(buf, room, ies->data, ies->len, true,
				     &truncated);
	if (truncated) {
		rcu_read_unlock();
		kfree(buf);
		atomic64_inc(&wlan->stats.bss_dropped);
		return;
	}

	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_BSS,
					  sizeof(*e) + n, GFP_ATOMIC);
	if (ev) {
		e = vduse_guest_wlan_event_payload(ev);
		e->id = cpu_to_le32(id);
		vduse_guest_wlan_bss_info(bss, ies, &e->bss, n);
		memcpy(e + 1, buf, n);
	}
	rcu_read_unlock();
	kfree(buf);

	if (!ev) {
		atomic64_inc(&wlan->stats.bss_dropped);
		return;
	}
	vduse_guest_wlan_queue_event(wlan, ev, true);
}

/* Report the entries that the scan updated, then the scan's completion. */
static void vduse_guest_wlan_scan_done(struct vduse_guest_wlan *wlan,
				       struct vduse_guest_wlan_note *note)
{
	struct virtio_wlan_event_scan_done *e;
	struct vduse_guest_wlan_event *ev;
	int i;

	for (i = 0; i < note->n_scan_bss; i++) {
		if (note->scan_bss[i]->use_for & NL80211_BSS_USE_FOR_NORMAL)
			vduse_guest_wlan_report_bss(wlan, note->id,
						    note->scan_bss[i]);
	}

	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_SCAN_DONE,
					  sizeof(*e), GFP_KERNEL);
	if (!ev)
		return;
	e = vduse_guest_wlan_event_payload(ev);
	e->id = cpu_to_le32(note->id);
	e->aborted = note->aborted;
	vduse_guest_wlan_queue_event(wlan, ev, false);
}

/*
 * Keep only the elements that place a BSS on its channel and describe its
 * security, compacting the blob in place.
 */
static size_t vduse_guest_wlan_essential_elems(u8 *ies, size_t len)
{
	size_t in = 0, pos = 0;

	/* The blob is well-formed, since it passed the allowlist. */
	while (in + sizeof(struct element) <= len) {
		const struct element *elem = (const void *)(ies + in);
		size_t n = sizeof(*elem) + elem->datalen;
		bool keep;

		switch (elem->id) {
		case WLAN_EID_SSID:
		case WLAN_EID_DS_PARAMS:
		case WLAN_EID_RSN:
		case WLAN_EID_RSNX:
		case WLAN_EID_HT_OPERATION:
			keep = true;
			break;
		case WLAN_EID_EXTENSION:
			keep = elem->datalen &&
			       elem->data[0] == WLAN_EID_EXT_HE_OPERATION;
			break;
		default:
			keep = false;
		}
		/* The move cannot overwrite an element that is still unread. */
		if (keep) {
			memmove(ies + pos, elem, n);
			pos += n;
		}
		in += n;
	}
	return pos;
}

static u8 vduse_guest_wlan_timeout_reason(enum nl80211_timeout_reason r)
{
	switch (r) {
	case NL80211_TIMEOUT_SCAN:
		return VIRTIO_WLAN_TIMEOUT_SCAN;
	case NL80211_TIMEOUT_AUTH:
		return VIRTIO_WLAN_TIMEOUT_AUTH;
	case NL80211_TIMEOUT_ASSOC:
		return VIRTIO_WLAN_TIMEOUT_ASSOC;
	default:
		return VIRTIO_WLAN_TIMEOUT_UNSPECIFIED;
	}
}

/*
 * Build a connection result. One that does not fit in an event first loses
 * the BSS's elements other than the essential ones, then the tail of its
 * response IEs, then the tail of its request IEs.
 */
static void vduse_guest_wlan_connect_result(struct vduse_guest_wlan *wlan,
					    struct vduse_guest_wlan_note *note)
{
	size_t room = VDUSE_GUEST_WLAN_EVENT_SIZE -
		      sizeof(struct virtio_wlan_event_hdr) -
		      sizeof(struct virtio_wlan_event_connect_result);
	const struct cfg80211_bss_ies *ies;
	struct virtio_wlan_event_connect_result *e;
	struct virtio_wlan_bss_info info = {};
	size_t bss_n = 0, req_n, resp_n, cap;
	struct vduse_guest_wlan_event *ev;
	u8 *bss_ie = NULL, *req = NULL, *resp = NULL, *p;

	bss_ie = kmalloc(room, GFP_KERNEL);
	req = kmalloc(room, GFP_KERNEL);
	resp = kmalloc(room, GFP_KERNEL);
	if (!bss_ie || !req || !resp)
		goto out;

	if (note->bss) {
		rcu_read_lock();
		ies = rcu_dereference(note->bss->ies);
		bss_n = virtio_wlan_filter_elems(bss_ie, room, ies->data,
						 ies->len, true, NULL);
		vduse_guest_wlan_bss_info(note->bss, ies, &info, 0);
		rcu_read_unlock();
	}
	req_n = virtio_wlan_filter_elems(req, room, note->req_ie,
					 note->req_ie_len, false, NULL);
	resp_n = virtio_wlan_filter_elems(resp, room, note->resp_ie,
					  note->resp_ie_len, true, NULL);

	if (bss_n + req_n + resp_n > room)
		bss_n = vduse_guest_wlan_essential_elems(bss_ie, bss_n);
	if (bss_n + req_n + resp_n > room) {
		cap = room - min(room, bss_n + req_n);
		resp_n = virtio_wlan_filter_elems(resp, cap, note->resp_ie,
						  note->resp_ie_len, true, NULL);
	}
	if (bss_n + req_n + resp_n > room) {
		cap = room - min(room, bss_n);
		req_n = virtio_wlan_filter_elems(req, cap, note->req_ie,
						 note->req_ie_len, false, NULL);
	}

	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_CONNECT_RESULT,
					  sizeof(*e) + bss_n + req_n + resp_n,
					  GFP_KERNEL);
	if (!ev)
		goto out;

	e = vduse_guest_wlan_event_payload(ev);
	e->id = cpu_to_le32(note->id);
	if (note->status < 0)
		e->timeout_reason =
			vduse_guest_wlan_timeout_reason(note->timeout_reason);
	else
		e->status = cpu_to_le16(note->status);
	e->req_ie_len = cpu_to_le16(req_n);
	e->resp_ie_len = cpu_to_le16(resp_n);
	if (note->bss) {
		info.ie_len = cpu_to_le16(bss_n);
		e->bss = info;
	}

	p = (u8 *)(e + 1);
	memcpy(p, bss_ie, bss_n);
	memcpy(p + bss_n, req, req_n);
	memcpy(p + bss_n + req_n, resp, resp_n);
	vduse_guest_wlan_queue_event(wlan, ev, false);
out:
	kfree(bss_ie);
	kfree(req);
	kfree(resp);
}

static void vduse_guest_wlan_disconnected(struct vduse_guest_wlan *wlan,
					  struct vduse_guest_wlan_note *note)
{
	size_t room = VDUSE_GUEST_WLAN_EVENT_SIZE -
		      sizeof(struct virtio_wlan_event_hdr) -
		      sizeof(struct virtio_wlan_event_disconnected);
	struct virtio_wlan_event_disconnected *e;
	struct vduse_guest_wlan_event *ev;
	size_t n;
	u8 *buf;

	buf = kmalloc(room, GFP_KERNEL);
	if (!buf)
		return;
	n = virtio_wlan_filter_elems(buf, room, note->ie, note->ie_len,
				     !note->locally_generated, NULL);

	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_DISCONNECTED,
					  sizeof(*e) + n, GFP_KERNEL);
	if (ev) {
		e = vduse_guest_wlan_event_payload(ev);
		e->id = cpu_to_le32(note->id);
		e->reason = cpu_to_le16(note->reason);
		e->ie_len = cpu_to_le16(n);
		e->locally_generated = note->locally_generated;
		memcpy(e + 1, buf, n);
		vduse_guest_wlan_queue_event(wlan, ev, false);
	}
	kfree(buf);
}

static void vduse_guest_wlan_note_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(work, struct vduse_guest_wlan, note_work);
	struct vduse_guest_wlan_note *note;
	struct cfg80211_bss **bss;
	int n;

	for (;;) {
		spin_lock_irq(&wlan->lock);
		note = list_first_entry_or_null(&wlan->notes,
						struct vduse_guest_wlan_note,
						list);
		if (note)
			list_del(&note->list);
		spin_unlock_irq(&wlan->lock);
		if (!note)
			break;

		/* The client API cannot be called with the RTNL held. */
		if (note->type == VDUSE_GUEST_WLAN_NOTE_SCAN_DONE) {
			n = cfg80211_client_get_scan_bss(wlan->client, &bss);
			if (n >= 0) {
				note->scan_bss = bss;
				note->n_scan_bss = n;
			}
		}

		/*
		 * Scan and connection events read the BSS entries' channels,
		 * which the driver frees once the interface is gone. A
		 * disconnection still reaches the host, since mac80211 reports
		 * it as it stops the interface, before the gone notification.
		 */
		rtnl_lock();
		if (wlan->gone &&
		    (note->type == VDUSE_GUEST_WLAN_NOTE_SCAN_DONE ||
		     note->type == VDUSE_GUEST_WLAN_NOTE_CONNECT)) {
			rtnl_unlock();
			vduse_guest_wlan_free_note(wlan, note);
			continue;
		}

		switch (note->type) {
		case VDUSE_GUEST_WLAN_NOTE_SCAN_DONE:
			vduse_guest_wlan_scan_done(wlan, note);
			break;
		case VDUSE_GUEST_WLAN_NOTE_CONNECT:
			vduse_guest_wlan_connect_result(wlan, note);
			break;
		case VDUSE_GUEST_WLAN_NOTE_DISCONNECTED:
			vduse_guest_wlan_disconnected(wlan, note);
			break;
		case VDUSE_GUEST_WLAN_NOTE_EVENT:
			vduse_guest_wlan_queue_event(wlan, note->event, false);
			note->event = NULL;
			break;
		}
		rtnl_unlock();
		vduse_guest_wlan_free_note(wlan, note);
	}

	queue_work(wlan->wq, &wlan->event_work);
}

/* Move events from the backlog into the buffers that the host posted. */
static void vduse_guest_wlan_event_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(work, struct vduse_guest_wlan, event_work);
	struct kvec rkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct kvec wkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct vduse_guest_wlan_event *ev;
	struct vringh_kiov riov, wiov;
	struct iov_iter it;
	size_t room;
	u32 len;
	u16 head;
	int r;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));
	vringh_kiov_init(&wiov, wkvec, ARRAY_SIZE(wkvec));

	for (;;) {
		spin_lock_irq(&wlan->lock);
		ev = wlan->running ?
			list_first_entry_or_null(&wlan->backlog,
						 struct vduse_guest_wlan_event,
						 list) : NULL;
		spin_unlock_irq(&wlan->lock);
		if (!ev)
			break;

		r = vduse_guest_getdesc(wlan->dev, VIRTIO_WLAN_VQ_EVENT, &riov,
					&wiov, &head);
		if (r < 0)
			break;
		if (!r) {
			if (vduse_guest_notify_enable(wlan->dev,
						      VIRTIO_WLAN_VQ_EVENT))
				break;
			vduse_guest_notify_disable(wlan->dev,
						   VIRTIO_WLAN_VQ_EVENT);
			continue;
		}

		spin_lock_irq(&wlan->lock);
		list_del(&ev->list);
		wlan->backlog_len--;
		spin_unlock_irq(&wlan->lock);

		len = 0;
		room = vringh_kiov_length(&wiov);
		if (ev->len <= room) {
			iov_iter_kvec(&it, ITER_DEST, wiov.iov, wiov.used, room);
			if (copy_to_iter(ev->data, ev->len, &it) == ev->len)
				len = ev->len;
		}
		if (len)
			atomic64_inc(&wlan->stats.events);
		else
			atomic64_inc(&wlan->stats.events_dropped);
		vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_EVENT, head, len);
		kfree(ev);
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

static void vduse_guest_wlan_note_scan_done(void *priv, bool aborted)
{
	struct vduse_guest_wlan *wlan = priv;
	struct vduse_guest_wlan_note *note;

	note = kzalloc_obj(*note);
	spin_lock_irq(&wlan->lock);
	wlan->scan_active = false;
	if (note) {
		note->type = VDUSE_GUEST_WLAN_NOTE_SCAN_DONE;
		note->id = wlan->scan_id;
		note->aborted = aborted;
	}
	spin_unlock_irq(&wlan->lock);
	if (note)
		vduse_guest_wlan_add_note(wlan, note);
}

static void
vduse_guest_wlan_note_connect(void *priv,
			      const struct cfg80211_connect_resp_params *cr)
{
	struct vduse_guest_wlan *wlan = priv;
	struct vduse_guest_wlan_note *note;

	note = kzalloc_obj(*note);
	if (note) {
		note->type = VDUSE_GUEST_WLAN_NOTE_CONNECT;
		note->status = cr->status;
		note->timeout_reason = cr->timeout_reason;
		if (cr->links[0].bss) {
			note->bss = cr->links[0].bss;
			cfg80211_ref_bss(wlan->wiphy, note->bss);
		}
		if (cr->req_ie_len)
			note->req_ie = kmemdup(cr->req_ie, cr->req_ie_len,
					       GFP_KERNEL);
		if (cr->resp_ie_len)
			note->resp_ie = kmemdup(cr->resp_ie, cr->resp_ie_len,
						GFP_KERNEL);
		note->req_ie_len = note->req_ie ? cr->req_ie_len : 0;
		note->resp_ie_len = note->resp_ie ? cr->resp_ie_len : 0;
	}

	spin_lock_irq(&wlan->lock);
	if (cr->status != WLAN_STATUS_SUCCESS)
		wlan->conn_active = false;
	if (note)
		note->id = wlan->conn_id;
	spin_unlock_irq(&wlan->lock);
	if (note)
		vduse_guest_wlan_add_note(wlan, note);
}

static void vduse_guest_wlan_note_disconnected(void *priv, u16 reason,
					       const u8 *ie, size_t ie_len,
					       bool locally_generated)
{
	struct vduse_guest_wlan *wlan = priv;
	struct vduse_guest_wlan_note *note;

	note = kzalloc_obj(*note);
	if (note) {
		note->type = VDUSE_GUEST_WLAN_NOTE_DISCONNECTED;
		note->reason = reason;
		note->locally_generated = locally_generated;
		if (ie_len)
			note->ie = kmemdup(ie, ie_len, GFP_KERNEL);
		note->ie_len = note->ie ? ie_len : 0;
	}

	spin_lock_irq(&wlan->lock);
	wlan->conn_active = false;
	if (note)
		note->id = wlan->conn_id;
	spin_unlock_irq(&wlan->lock);
	if (note)
		vduse_guest_wlan_add_note(wlan, note);
}

static bool vduse_guest_wlan_same_mic(struct vduse_guest_wlan_event *ev,
				      const struct virtio_wlan_event_mic_failure *m)
{
	struct virtio_wlan_event_hdr *hdr = (void *)ev->data;

	return le16_to_cpu(hdr->type) == VIRTIO_WLAN_EVENT_MIC_FAILURE &&
	       !memcmp(vduse_guest_wlan_event_payload(ev), m, sizeof(*m));
}

/*
 * The air controls how often this runs, so at most one failure per peer and
 * key waits in the notes and the backlog.
 */
static void vduse_guest_wlan_note_mic_failure(void *priv, const u8 *addr,
					      enum nl80211_key_type key_type,
					      int key_id, const u8 *tsc)
{
	struct vduse_guest_wlan *wlan = priv;
	struct virtio_wlan_event_mic_failure m = {};
	struct vduse_guest_wlan_note *note;
	struct vduse_guest_wlan_event *ev;
	unsigned long flags;
	bool queued = false;

	ether_addr_copy(m.peer, addr);
	m.key_index = key_id < 0 ? VIRTIO_WLAN_KEY_INDEX_NONE : key_id;
	m.pairwise = key_type == NL80211_KEYTYPE_PAIRWISE;

	spin_lock_irqsave(&wlan->lock, flags);
	list_for_each_entry(ev, &wlan->backlog, list)
		queued |= vduse_guest_wlan_same_mic(ev, &m);
	list_for_each_entry(note, &wlan->notes, list)
		queued |= note->event && vduse_guest_wlan_same_mic(note->event,
								   &m);
	spin_unlock_irqrestore(&wlan->lock, flags);
	if (queued)
		return;

	note = kzalloc_obj(*note, GFP_ATOMIC);
	if (!note)
		return;
	note->type = VDUSE_GUEST_WLAN_NOTE_EVENT;
	note->event = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_MIC_FAILURE,
						   sizeof(m), GFP_ATOMIC);
	if (!note->event) {
		kfree(note);
		return;
	}
	memcpy(vduse_guest_wlan_event_payload(note->event), &m, sizeof(m));
	vduse_guest_wlan_add_note(wlan, note);
}

static bool vduse_guest_wlan_data_running(struct vduse_guest_wlan *wlan)
{
	bool running;

	spin_lock_bh(&wlan->data_lock);
	running = wlan->data_running;
	spin_unlock_bh(&wlan->data_lock);
	return running;
}

/*
 * A driver can free a frame at any time, also after the device has stopped.
 * This reads data_running, clears tx_wake and queues the work under
 * data_lock, so that stopping the data path either cancels the work or keeps
 * it from being queued.
 */
static void vduse_guest_wlan_tx_release(struct vduse_guest_wlan *wlan)
{
	unsigned long flags;

	atomic_dec(&wlan->tx_pending);
	/* This pairs with the barrier in vduse_guest_wlan_tx_full(). */
	smp_mb__after_atomic();
	if (!test_bit(0, &wlan->tx_wake))
		return;
	spin_lock_irqsave(&wlan->data_lock, flags);
	if (wlan->data_running && test_and_clear_bit(0, &wlan->tx_wake))
		queue_work(system_percpu_wq, &wlan->tx_work);
	spin_unlock_irqrestore(&wlan->data_lock, flags);
}

static void vduse_guest_wlan_tx_destructor(struct sk_buff *skb)
{
	vduse_guest_wlan_tx_release(skb_shinfo(skb)->destructor_arg);
}

/*
 * A control port frame completes its chain once it leaves the guest's stack,
 * so that the host sends a new key only after message 4 of the 4-way
 * handshake has been sent with the old one.
 */
static void vduse_guest_wlan_tx_chain_destructor(struct sk_buff *skb)
{
	struct vduse_guest_wlan_tx_chain *chain =
		skb_shinfo(skb)->destructor_arg;
	struct vduse_guest_wlan *wlan = chain->wlan;
	unsigned long flags;

	spin_lock_irqsave(&wlan->data_lock, flags);
	if ((wlan->data_running || wlan->tx_drain) &&
	    chain->gen == wlan->tx_gen) {
		vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_TX, chain->head,
				     0);
		atomic64_inc(&wlan->stats.tx_chained);
	}
	spin_unlock_irqrestore(&wlan->data_lock, flags);
	kfree(chain);
	vduse_guest_wlan_tx_release(wlan);
}

/*
 * The largest frame in either direction, as the configuration's MTU allows.
 * A VLAN tag can take a frame past the MTU.
 */
static size_t vduse_guest_wlan_max_frame(struct vduse_guest_wlan *wlan)
{
	return VLAN_ETH_HLEN + le16_to_cpu(wlan->config.mtu);
}

/*
 * Send a frame from the host. No offload is offered, so its header is zero,
 * and its source must be the interface's own address. Return whether the
 * frame's destructor owns the chain's completion, as a control port frame's
 * does while the data path runs and its chain could be allocated.
 */
static bool vduse_guest_wlan_tx_one(struct vduse_guest_wlan *wlan,
				    struct vringh_kiov *riov, u16 head)
{
	struct net_device *netdev = wlan->netdev;
	size_t len = vringh_kiov_length(riov);
	struct vduse_guest_wlan_tx_chain *chain = NULL;
	struct virtio_net_hdr_v1 hdr;
	bool chained;
	struct sk_buff *skb;
	struct iov_iter it;
	size_t frame_len;

	if (len < sizeof(hdr) + ETH_HLEN ||
	    len > sizeof(hdr) + vduse_guest_wlan_max_frame(wlan)) {
		atomic64_inc(&wlan->stats.tx_invalid);
		return false;
	}
	frame_len = len - sizeof(hdr);
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, len);
	if (copy_from_iter(&hdr, sizeof(hdr), &it) != sizeof(hdr) ||
	    hdr.flags || hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		atomic64_inc(&wlan->stats.tx_invalid);
		return false;
	}

	skb = alloc_skb(LL_RESERVED_SPACE(netdev) + frame_len +
			netdev->needed_tailroom, GFP_KERNEL);
	if (!skb) {
		atomic64_inc(&wlan->stats.tx_dropped);
		return false;
	}
	skb_reserve(skb, LL_RESERVED_SPACE(netdev));
	if (copy_from_iter(skb_put(skb, frame_len), frame_len, &it) !=
	    frame_len) {
		kfree_skb(skb);
		atomic64_inc(&wlan->stats.tx_invalid);
		return false;
	}

	skb_reset_mac_header(skb);
	if (!ether_addr_equal(eth_hdr(skb)->h_source, netdev->dev_addr)) {
		kfree_skb(skb);
		atomic64_inc(&wlan->stats.tx_spoofed);
		return false;
	}

	/*
	 * Classification reads the IP header through the network header and
	 * the protocol, so an IP frame must hold one.
	 */
	skb_set_network_header(skb, ETH_HLEN);
	skb->protocol = eth_hdr(skb)->h_proto;
	if ((skb->protocol == htons(ETH_P_IP) &&
	     frame_len < ETH_HLEN + sizeof(struct iphdr)) ||
	    (skb->protocol == htons(ETH_P_IPV6) &&
	     frame_len < ETH_HLEN + sizeof(struct ipv6hdr))) {
		kfree_skb(skb);
		atomic64_inc(&wlan->stats.tx_invalid);
		return false;
	}
	skb->dev = netdev;

	if (skb->protocol == READ_ONCE(wlan->control_port))
		chain = kmalloc_obj(*chain);
	if (chain) {
		chain->wlan = wlan;
		chain->head = head;
		spin_lock_bh(&wlan->data_lock);
		chain->gen = wlan->tx_gen;
		if (!wlan->data_running) {
			kfree(chain);
			chain = NULL;
		}
		spin_unlock_bh(&wlan->data_lock);
	}
	/* The destructor can free the chain before dev_queue_xmit() returns. */
	chained = chain;
	if (chain) {
		skb_shinfo(skb)->destructor_arg = chain;
		skb->destructor = vduse_guest_wlan_tx_chain_destructor;
	} else {
		skb_shinfo(skb)->destructor_arg = wlan;
		skb->destructor = vduse_guest_wlan_tx_destructor;
	}
	atomic_inc(&wlan->tx_pending);
	atomic64_inc(&wlan->stats.tx_frames);
	dev_queue_xmit(skb);
	return chained;
}

/*
 * Stop at the limit of frames in mac80211, with notifications disabled, so
 * that the destructor that frees room restarts the work.
 */
static bool vduse_guest_wlan_tx_full(struct vduse_guest_wlan *wlan)
{
	if (atomic_read(&wlan->tx_pending) < VDUSE_GUEST_WLAN_TX_LIMIT)
		return false;
	set_bit(0, &wlan->tx_wake);
	/*
	 * A destructor that ran before the flag was set is seen in the count
	 * read below, and one after it sees the flag.
	 */
	smp_mb__after_atomic();
	if (atomic_read(&wlan->tx_pending) < VDUSE_GUEST_WLAN_TX_LIMIT &&
	    test_and_clear_bit(0, &wlan->tx_wake))
		return false;
	return true;
}

/* Return whether the tx work may use the queue, and whether it only drains. */
static bool vduse_guest_wlan_tx_usable(struct vduse_guest_wlan *wlan,
				       bool *drain)
{
	bool usable;

	spin_lock_bh(&wlan->data_lock);
	usable = wlan->data_running || wlan->tx_drain;
	*drain = !wlan->data_running;
	spin_unlock_bh(&wlan->data_lock);
	return usable;
}

/*
 * The host's queue would stop for good once the interface is gone, so the
 * frames are then completed unsent until the device stops.
 */
static void vduse_guest_wlan_tx_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(work, struct vduse_guest_wlan, tx_work);
	struct kvec rkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct kvec wkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct vringh_kiov riov, wiov;
	bool drain;
	u16 head;
	int r;

	if (!vduse_guest_wlan_tx_usable(wlan, &drain))
		return;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));
	vringh_kiov_init(&wiov, wkvec, ARRAY_SIZE(wkvec));

	vduse_guest_notify_disable(wlan->dev, VIRTIO_WLAN_VQ_TX);
	for (;;) {
		if (!vduse_guest_wlan_tx_usable(wlan, &drain) ||
		    (!drain && vduse_guest_wlan_tx_full(wlan)))
			break;

		r = vduse_guest_getdesc(wlan->dev, VIRTIO_WLAN_VQ_TX, &riov,
					&wiov, &head);
		if (r < 0)
			break;
		if (!r) {
			if (vduse_guest_poll(wlan->dev, VIRTIO_WLAN_VQ_TX))
				continue;
			if (vduse_guest_notify_enable(wlan->dev,
						      VIRTIO_WLAN_VQ_TX))
				break;
			vduse_guest_notify_disable(wlan->dev, VIRTIO_WLAN_VQ_TX);
			continue;
		}

		if (drain)
			atomic64_inc(&wlan->stats.tx_dropped);
		else if (vduse_guest_wlan_tx_one(wlan, &riov, head))
			continue;
		vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_TX, head, 0);
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

static void vduse_guest_wlan_free_rx_buf(struct vduse_guest_wlan_rx_buf *buf)
{
	vringh_kiov_cleanup(&buf->wiov);
	kfree(buf);
}

/*
 * Take the buffers that the host posts, which can sleep, so that the rx
 * handler only copies into them. A buffer too small for the largest frame
 * is returned unused. An allocation that fails is retried later, with
 * notifications still disabled.
 */
static void vduse_guest_wlan_rx_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(to_delayed_work(work), struct vduse_guest_wlan,
			     rx_work);
	struct kvec rkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct vduse_guest_wlan_rx_buf *buf;
	struct vringh_kiov riov;
	bool running;
	int r;

	if (!vduse_guest_wlan_data_running(wlan))
		return;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));

	vduse_guest_notify_disable(wlan->dev, VIRTIO_WLAN_VQ_RX);
	for (;;) {
		if (!vduse_guest_wlan_data_running(wlan))
			break;

		buf = kzalloc_obj(*buf);
		if (!buf) {
			schedule_delayed_work(&wlan->rx_work,
					      VDUSE_GUEST_WLAN_RETRY_DELAY);
			break;
		}
		vringh_kiov_init(&buf->wiov, buf->kvec, ARRAY_SIZE(buf->kvec));

		r = vduse_guest_getdesc(wlan->dev, VIRTIO_WLAN_VQ_RX, &riov,
					&buf->wiov, &buf->head);
		if (r <= 0) {
			vduse_guest_wlan_free_rx_buf(buf);
			if (r < 0)
				break;
			if (vduse_guest_notify_enable(wlan->dev,
						      VIRTIO_WLAN_VQ_RX))
				break;
			vduse_guest_notify_disable(wlan->dev, VIRTIO_WLAN_VQ_RX);
			continue;
		}

		if (vringh_kiov_length(&buf->wiov) <
		    sizeof(struct virtio_net_hdr_v1) +
		    vduse_guest_wlan_max_frame(wlan)) {
			atomic64_inc(&wlan->stats.rx_bad_buffers);
			vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_RX,
					     buf->head, 0);
			vduse_guest_wlan_free_rx_buf(buf);
			continue;
		}

		/* The data path can stop while the buffer is taken. */
		spin_lock_bh(&wlan->data_lock);
		running = wlan->data_running;
		if (running)
			list_add_tail(&buf->list, &wlan->rx_bufs);
		spin_unlock_bh(&wlan->data_lock);
		if (!running) {
			vduse_guest_wlan_free_rx_buf(buf);
			break;
		}
	}

	vringh_kiov_cleanup(&riov);
}

/*
 * Copy a frame from its MAC header. The stack has already moved a VLAN tag
 * out of the frame, so the tag goes back after the addresses.
 */
static int vduse_guest_wlan_copy_frame(struct sk_buff *skb,
				       struct iov_iter *it, size_t frame_len)
{
	int offset = skb_mac_offset(skb);
	__be16 tag[2];

	if (!skb_vlan_tag_present(skb))
		return skb_copy_datagram_iter(skb, offset, it, frame_len);

	tag[0] = skb->vlan_proto;
	tag[1] = htons(skb_vlan_tag_get(skb));
	if (skb_copy_datagram_iter(skb, offset, it, 2 * ETH_ALEN) ||
	    copy_to_iter(tag, sizeof(tag), it) != sizeof(tag))
		return -EFAULT;
	return skb_copy_datagram_iter(skb, offset + 2 * ETH_ALEN, it,
				      frame_len - VLAN_HLEN - 2 * ETH_ALEN);
}

/*
 * Every frame received on the interface is consumed, and the guest's own
 * stack sees none. A frame is copied into the next buffer, which holds the
 * largest frame, and completed at once. A stopping device waits for this
 * handler to return.
 */
static rx_handler_result_t vduse_guest_wlan_rx(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct vduse_guest_wlan *wlan =
		rcu_dereference(skb->dev->rx_handler_data);
	struct virtio_net_hdr_v1 hdr = {
		.num_buffers = cpu_to_le16(1),
	};
	struct vduse_guest_wlan_rx_buf *buf;
	size_t frame_len = skb->len - skb_mac_offset(skb);
	struct iov_iter it;

	if (skb_vlan_tag_present(skb))
		frame_len += VLAN_HLEN;
	if (frame_len > vduse_guest_wlan_max_frame(wlan)) {
		atomic64_inc(&wlan->stats.rx_oversized);
		goto drop;
	}

	spin_lock(&wlan->data_lock);
	buf = wlan->data_running ?
		list_first_entry_or_null(&wlan->rx_bufs,
					 struct vduse_guest_wlan_rx_buf, list) :
		NULL;
	if (buf)
		list_del(&buf->list);
	spin_unlock(&wlan->data_lock);
	if (!buf) {
		atomic64_inc(&wlan->stats.rx_no_buffer);
		goto drop;
	}

	iov_iter_kvec(&it, ITER_DEST, buf->wiov.iov, buf->wiov.used,
		      vringh_kiov_length(&buf->wiov));
	if (copy_to_iter(&hdr, sizeof(hdr), &it) != sizeof(hdr) ||
	    vduse_guest_wlan_copy_frame(skb, &it, frame_len)) {
		atomic64_inc(&wlan->stats.rx_bad_buffers);
		vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_RX, buf->head, 0);
		vduse_guest_wlan_free_rx_buf(buf);
		goto drop;
	}
	atomic64_inc(&wlan->stats.rx_frames);
	vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_RX, buf->head,
			     sizeof(hdr) + frame_len);
	vduse_guest_wlan_free_rx_buf(buf);
	consume_skb(skb);
	return RX_HANDLER_CONSUMED;
drop:
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

/*
 * Stop the data path. Frames that mac80211 still holds keep their count, and
 * the tx work of the next start sets tx_wake again if they still fill the
 * limit. The buffers that the host posted are dropped without being
 * completed, and a reset returns them to the driver. With drain set, a
 * running device keeps draining the tx queue until it stops, from now on if
 * the queue has been kicked and from its first kick otherwise.
 */
static void vduse_guest_wlan_stop_data(struct vduse_guest_wlan *wlan,
				       bool drain)
{
	struct vduse_guest_wlan_rx_buf *buf, *tmp;
	LIST_HEAD(bufs);

	spin_lock_bh(&wlan->data_lock);
	drain = drain && wlan->data_running;
	wlan->tx_drain = drain;
	wlan->data_running = false;
	if (!drain)
		wlan->tx_gen++;
	list_splice_init(&wlan->rx_bufs, &bufs);
	spin_unlock_bh(&wlan->data_lock);

	cancel_delayed_work_sync(&wlan->rx_work);
	cancel_work_sync(&wlan->tx_work);
	/* A tx work that was still running can have set the flag again. */
	clear_bit(0, &wlan->tx_wake);
	list_for_each_entry_safe(buf, tmp, &bufs, list)
		vduse_guest_wlan_free_rx_buf(buf);

	/*
	 * A stopping device forgets the kick, and a draining one starts the
	 * drain now if the queue has been kicked.
	 */
	if (!drain)
		clear_bit(0, &wlan->tx_kicked);
	else if (test_bit(0, &wlan->tx_kicked))
		queue_work(system_percpu_wq, &wlan->tx_work);
}

/* This can be called in atomic context, so the report is built by a work. */
static void vduse_guest_wlan_reg_change(void *priv)
{
	struct vduse_guest_wlan *wlan = priv;

	queue_work(wlan->wq, &wlan->country_work);
}

/*
 * The interface is being unregistered, with the RTNL and the wiphy lock.
 * cfg80211 forgets the client before it ends the connection, so the host
 * is told of the disconnection here.
 */
static void vduse_guest_wlan_gone(void *priv)
{
	struct vduse_guest_wlan *wlan = priv;
	struct virtio_wlan_event_disconnected *e;
	struct vduse_guest_wlan_event *ev;
	bool connected;
	u32 id;

	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_DISCONNECTED,
					  sizeof(*e), GFP_KERNEL);
	spin_lock_irq(&wlan->lock);
	connected = ev && wlan->running && wlan->conn_active;
	id = wlan->conn_id;
	if (connected)
		wlan->conn_active = false;
	spin_unlock_irq(&wlan->lock);
	if (connected) {
		e = vduse_guest_wlan_event_payload(ev);
		e->id = cpu_to_le32(id);
		e->reason = cpu_to_le16(WLAN_REASON_UNSPECIFIED);
		e->locally_generated = 1;
		vduse_guest_wlan_queue_event(wlan, ev, false);
		queue_work(wlan->wq, &wlan->event_work);
	} else {
		kfree(ev);
	}

	wlan->gone = true;
	if (wlan->rx_registered) {
		netdev_rx_handler_unregister(wlan->netdev);
		wlan->rx_registered = false;
	}
	vduse_guest_wlan_stop_data(wlan, true);
	pr_warn("%s: interface %s is gone\n", vduse_guest_name(wlan->dev),
		wlan->netdev->name);
}

static const struct cfg80211_client_ops vduse_guest_wlan_client_ops = {
	.scan_done = vduse_guest_wlan_note_scan_done,
	.connect_result = vduse_guest_wlan_note_connect,
	.disconnected = vduse_guest_wlan_note_disconnected,
	.mic_failure = vduse_guest_wlan_note_mic_failure,
	.reg_change = vduse_guest_wlan_reg_change,
	.gone = vduse_guest_wlan_gone,
};

static int vduse_guest_wlan_cmd_scan(struct vduse_guest_wlan *wlan,
				     const u8 *p, size_t len)
{
	const struct virtio_wlan_scan *s = (const void *)p;
	struct cfg80211_client_scan_params params = {};
	struct cfg80211_ssid *ssids = NULL;
	const struct virtio_wlan_ssid *ws;
	unsigned int n_ssids, n_freqs, ie_len, i;
	const __le16 *wfreqs;
	u32 *freqs = NULL;
	u32 id, flags;
	int err;

	if (len < sizeof(*s))
		return -EINVAL;
	n_ssids = le16_to_cpu(s->n_ssids);
	n_freqs = le16_to_cpu(s->n_freqs);
	ie_len = le16_to_cpu(s->ie_len);
	if (len != sizeof(*s) + n_ssids * sizeof(*ws) +
		   n_freqs * sizeof(*wfreqs) + ie_len)
		return -EINVAL;

	flags = le32_to_cpu(s->flags);
	if (flags & ~VIRTIO_WLAN_SCAN_F_RANDOM_MAC)
		return -EINVAL;

	ws = (const void *)(s + 1);
	wfreqs = (const void *)(ws + n_ssids);

	ssids = kcalloc(n_ssids, sizeof(*ssids), GFP_KERNEL);
	freqs = kcalloc(n_freqs, sizeof(*freqs), GFP_KERNEL);
	if ((n_ssids && !ssids) || (n_freqs && !freqs)) {
		err = -ENOMEM;
		goto out;
	}

	for (i = 0; i < n_ssids; i++) {
		if (ws[i].len > IEEE80211_MAX_SSID_LEN) {
			err = -EINVAL;
			goto out;
		}
		ssids[i].ssid_len = ws[i].len;
		memcpy(ssids[i].ssid, ws[i].ssid, ws[i].len);
	}
	for (i = 0; i < n_freqs; i++)
		freqs[i] = le16_to_cpu(wfreqs[i]);

	params.ssids = n_ssids ? ssids : NULL;
	params.n_ssids = n_ssids;
	params.freqs = n_freqs ? freqs : NULL;
	params.n_freqs = n_freqs;
	params.ie = ie_len ? (const u8 *)(wfreqs + n_freqs) : NULL;
	params.ie_len = ie_len;
	if (flags & VIRTIO_WLAN_SCAN_F_RANDOM_MAC) {
		params.flags |= NL80211_SCAN_FLAG_RANDOM_ADDR;
		ether_addr_copy(params.mac_addr, s->mac_addr);
		ether_addr_copy(params.mac_addr_mask, s->mac_addr_mask);
	}
	params.bssid = is_zero_ether_addr(s->bssid) ? NULL : s->bssid;

	/* The id is set first, since the scan can end before the call returns. */
	id = le32_to_cpu(s->id);
	spin_lock_irq(&wlan->lock);
	if (wlan->scan_active) {
		spin_unlock_irq(&wlan->lock);
		err = -EBUSY;
		goto out;
	}
	wlan->scan_active = true;
	wlan->scan_id = id;
	spin_unlock_irq(&wlan->lock);

	err = cfg80211_client_scan(wlan->client, &params);
	if (err) {
		spin_lock_irq(&wlan->lock);
		if (wlan->scan_id == id)
			wlan->scan_active = false;
		spin_unlock_irq(&wlan->lock);
	}
out:
	kfree(ssids);
	kfree(freqs);
	return err;
}

/* Aborting a scan other than the current one does nothing. */
static int vduse_guest_wlan_cmd_abort_scan(struct vduse_guest_wlan *wlan,
					   const u8 *p, size_t len)
{
	const struct virtio_wlan_abort_scan *a = (const void *)p;
	bool current_scan;
	int err;

	if (len != sizeof(*a))
		return -EINVAL;

	spin_lock_irq(&wlan->lock);
	current_scan = wlan->scan_active &&
		       wlan->scan_id == le32_to_cpu(a->id);
	spin_unlock_irq(&wlan->lock);
	if (!current_scan)
		return 0;

	err = cfg80211_client_abort_scan(wlan->client);
	return err == -ENOENT ? 0 : err;
}

static int vduse_guest_wlan_auth_type(u8 type, enum nl80211_auth_type *out)
{
	switch (type) {
	case VIRTIO_WLAN_AUTH_OPEN:
		*out = NL80211_AUTHTYPE_OPEN_SYSTEM;
		return 0;
	case VIRTIO_WLAN_AUTH_SHARED_KEY:
		*out = NL80211_AUTHTYPE_SHARED_KEY;
		return 0;
	case VIRTIO_WLAN_AUTH_AUTOMATIC:
		*out = NL80211_AUTHTYPE_AUTOMATIC;
		return 0;
	case VIRTIO_WLAN_AUTH_SAE:
		/* SAE needs external authentication, which is not offered. */
		return -EOPNOTSUPP;
	default:
		return -EINVAL;
	}
}

static int vduse_guest_wlan_mfp(u8 mfp, enum nl80211_mfp *out)
{
	switch (mfp) {
	case VIRTIO_WLAN_MFP_NO:
		*out = NL80211_MFP_NO;
		return 0;
	case VIRTIO_WLAN_MFP_REQUIRED:
		*out = NL80211_MFP_REQUIRED;
		return 0;
	case VIRTIO_WLAN_MFP_OPTIONAL:
		*out = NL80211_MFP_OPTIONAL;
		return 0;
	default:
		return -EINVAL;
	}
}

#define VDUSE_GUEST_WLAN_CONNECT_FLAGS			\
	(VIRTIO_WLAN_CONNECT_F_PRIVACY |		\
	 VIRTIO_WLAN_CONNECT_F_CONTROL_PORT |		\
	 VIRTIO_WLAN_CONNECT_F_CONTROL_PORT_NO_ENCRYPT |	\
	 VIRTIO_WLAN_CONNECT_F_DISABLE_HT |		\
	 VIRTIO_WLAN_CONNECT_F_DISABLE_VHT |		\
	 VIRTIO_WLAN_CONNECT_F_DISABLE_HE |		\
	 VIRTIO_WLAN_CONNECT_F_DISABLE_EHT)

static int vduse_guest_wlan_connect_params(struct vduse_guest_wlan *wlan,
					   const struct virtio_wlan_connect *c,
					   struct cfg80211_connect_params *params)
{
	struct cfg80211_crypto_settings *crypto = &params->crypto;
	u32 flags = le32_to_cpu(c->flags);
	u32 wpa = le32_to_cpu(c->wpa_versions);
	u16 freq, freq_hint;
	int err, i;

	if (!c->ssid_len || c->ssid_len > IEEE80211_MAX_SSID_LEN ||
	    c->n_ciphers_pairwise > VIRTIO_WLAN_MAX_PAIRWISE ||
	    c->n_akm_suites > VIRTIO_WLAN_MAX_AKM_SUITES ||
	    c->key_len > VIRTIO_WLAN_WEP_KEY_LEN ||
	    (flags & ~VDUSE_GUEST_WLAN_CONNECT_FLAGS) ||
	    (wpa & ~(VIRTIO_WLAN_WPA_VERSION_1 | VIRTIO_WLAN_WPA_VERSION_2 |
		     VIRTIO_WLAN_WPA_VERSION_3)))
		return -EINVAL;

	err = vduse_guest_wlan_auth_type(c->auth_type, &params->auth_type);
	if (err)
		return err;
	err = vduse_guest_wlan_mfp(c->mfp, &params->mfp);
	if (err)
		return err;

	params->ssid = c->ssid;
	params->ssid_len = c->ssid_len;
	if (!is_zero_ether_addr(c->bssid))
		params->bssid = c->bssid;
	if (!is_zero_ether_addr(c->bssid_hint))
		params->bssid_hint = c->bssid_hint;

	/* The driver frees its channels once the interface is gone. */
	freq = le16_to_cpu(c->freq);
	freq_hint = le16_to_cpu(c->freq_hint);
	rtnl_lock();
	if (wlan->gone) {
		rtnl_unlock();
		return -ENODEV;
	}
	if (freq)
		params->channel = ieee80211_get_channel(wlan->wiphy, freq);
	if (freq_hint)
		params->channel_hint = ieee80211_get_channel(wlan->wiphy,
							     freq_hint);
	rtnl_unlock();
	if ((freq && !params->channel) || (freq_hint && !params->channel_hint))
		return -EINVAL;

	params->privacy = flags & VIRTIO_WLAN_CONNECT_F_PRIVACY;
	if (wpa & VIRTIO_WLAN_WPA_VERSION_1)
		crypto->wpa_versions |= NL80211_WPA_VERSION_1;
	if (wpa & VIRTIO_WLAN_WPA_VERSION_2)
		crypto->wpa_versions |= NL80211_WPA_VERSION_2;
	if (wpa & VIRTIO_WLAN_WPA_VERSION_3)
		crypto->wpa_versions |= NL80211_WPA_VERSION_3;
	crypto->cipher_group = le32_to_cpu(c->cipher_group);
	crypto->n_ciphers_pairwise = c->n_ciphers_pairwise;
	for (i = 0; i < c->n_ciphers_pairwise; i++)
		crypto->ciphers_pairwise[i] =
			le32_to_cpu(c->ciphers_pairwise[i]);
	crypto->n_akm_suites = c->n_akm_suites;
	for (i = 0; i < c->n_akm_suites; i++)
		crypto->akm_suites[i] = le32_to_cpu(c->akm_suites[i]);
	crypto->control_port = flags & VIRTIO_WLAN_CONNECT_F_CONTROL_PORT;
	crypto->control_port_ethertype =
		cpu_to_be16(le16_to_cpu(c->control_port_ethertype));
	crypto->control_port_no_encrypt =
		flags & VIRTIO_WLAN_CONNECT_F_CONTROL_PORT_NO_ENCRYPT;

	if (flags & VIRTIO_WLAN_CONNECT_F_DISABLE_HT)
		params->flags |= ASSOC_REQ_DISABLE_HT;
	if (flags & VIRTIO_WLAN_CONNECT_F_DISABLE_VHT)
		params->flags |= ASSOC_REQ_DISABLE_VHT;
	if (flags & VIRTIO_WLAN_CONNECT_F_DISABLE_HE)
		params->flags |= ASSOC_REQ_DISABLE_HE;
	if (flags & VIRTIO_WLAN_CONNECT_F_DISABLE_EHT)
		params->flags |= ASSOC_REQ_DISABLE_EHT;

	if (c->key_len) {
		params->key = c->key;
		params->key_len = c->key_len;
		params->key_idx = c->key_idx;
	}
	params->bg_scan_period = -1;
	return 0;
}

static int vduse_guest_wlan_cmd_connect(struct vduse_guest_wlan *wlan,
					const u8 *p, size_t len)
{
	const struct virtio_wlan_connect *c = (const void *)p;
	struct cfg80211_connect_params params = {};
	unsigned int ie_len;
	u32 id, prev_id;
	int err;

	if (len < sizeof(*c))
		return -EINVAL;
	ie_len = le16_to_cpu(c->ie_len);
	if (len != sizeof(*c) + ie_len)
		return -EINVAL;

	err = vduse_guest_wlan_connect_params(wlan, c, &params);
	if (err)
		return err;
	params.ie = ie_len ? (const u8 *)(c + 1) : NULL;
	params.ie_len = ie_len;

	/*
	 * The id is set first, since the attempt can end before the call
	 * returns.
	 */
	id = le32_to_cpu(c->id);
	spin_lock_irq(&wlan->lock);
	if (wlan->conn_active) {
		spin_unlock_irq(&wlan->lock);
		return -EALREADY;
	}
	prev_id = wlan->conn_id;
	wlan->conn_active = true;
	wlan->conn_id = id;
	spin_unlock_irq(&wlan->lock);
	/* The client API takes an ethertype of 0 as ETH_P_PAE. */
	WRITE_ONCE(wlan->control_port,
		   params.crypto.control_port_ethertype ?: htons(ETH_P_PAE));

	err = cfg80211_client_connect(wlan->client, &params);
	if (err) {
		spin_lock_irq(&wlan->lock);
		if (wlan->conn_id == id) {
			wlan->conn_active = false;
			wlan->conn_id = prev_id;
		}
		spin_unlock_irq(&wlan->lock);
	}
	return err;
}

/*
 * Disconnecting from a connection other than the current one does nothing.
 * mac80211 reports the disconnection while the call runs, so it still
 * carries this connection's id.
 */
static int vduse_guest_wlan_cmd_disconnect(struct vduse_guest_wlan *wlan,
					   const u8 *p, size_t len)
{
	const struct virtio_wlan_disconnect *d = (const void *)p;
	bool current_conn;
	int err;

	if (len != sizeof(*d))
		return -EINVAL;

	spin_lock_irq(&wlan->lock);
	current_conn = wlan->conn_active &&
		       wlan->conn_id == le32_to_cpu(d->id);
	spin_unlock_irq(&wlan->lock);
	if (!current_conn)
		return 0;

	err = cfg80211_client_disconnect(wlan->client,
					 le16_to_cpu(d->reason));
	if (!err) {
		spin_lock_irq(&wlan->lock);
		if (wlan->conn_id == le32_to_cpu(d->id))
			wlan->conn_active = false;
		spin_unlock_irq(&wlan->lock);
	}
	return err;
}

static const u8 *vduse_guest_wlan_peer(const u8 *addr)
{
	return is_zero_ether_addr(addr) ? NULL : addr;
}

static int vduse_guest_wlan_cmd_add_key(struct vduse_guest_wlan *wlan,
					const u8 *p, size_t len)
{
	const struct virtio_wlan_add_key *k = (const void *)p;
	struct key_params params = {};

	if (len != sizeof(*k) || (k->flags & ~VIRTIO_WLAN_KEY_F_PAIRWISE) ||
	    k->key_len > VIRTIO_WLAN_KEY_LEN || k->seq_len > VIRTIO_WLAN_SEQ_LEN)
		return -EINVAL;

	params.key = k->key;
	params.key_len = k->key_len;
	params.seq = k->seq_len ? k->seq : NULL;
	params.seq_len = k->seq_len;
	params.cipher = le32_to_cpu(k->cipher);
	return cfg80211_client_add_key(wlan->client, k->index,
				       k->flags & VIRTIO_WLAN_KEY_F_PAIRWISE,
				       vduse_guest_wlan_peer(k->peer), &params);
}

static int vduse_guest_wlan_cmd_del_key(struct vduse_guest_wlan *wlan,
					const u8 *p, size_t len)
{
	const struct virtio_wlan_del_key *k = (const void *)p;

	if (len != sizeof(*k) || (k->flags & ~VIRTIO_WLAN_KEY_F_PAIRWISE))
		return -EINVAL;

	return cfg80211_client_del_key(wlan->client, k->index,
				       k->flags & VIRTIO_WLAN_KEY_F_PAIRWISE,
				       vduse_guest_wlan_peer(k->peer));
}

static int vduse_guest_wlan_cmd_default_key(struct vduse_guest_wlan *wlan,
					    const u8 *p, size_t len)
{
	const struct virtio_wlan_set_default_key *k = (const void *)p;

	if (len != sizeof(*k) ||
	    (k->flags & ~(VIRTIO_WLAN_DEFAULT_KEY_F_UNICAST |
			  VIRTIO_WLAN_DEFAULT_KEY_F_MULTICAST)))
		return -EINVAL;

	return cfg80211_client_set_default_key(wlan->client, k->index,
				k->flags & VIRTIO_WLAN_DEFAULT_KEY_F_UNICAST,
				k->flags & VIRTIO_WLAN_DEFAULT_KEY_F_MULTICAST);
}

static int vduse_guest_wlan_cmd_default_mgmt_key(struct vduse_guest_wlan *wlan,
						 const u8 *p, size_t len)
{
	const struct virtio_wlan_set_default_mgmt_key *k = (const void *)p;

	if (len != sizeof(*k))
		return -EINVAL;
	return cfg80211_client_set_default_mgmt_key(wlan->client, k->index);
}

static int vduse_guest_wlan_cmd_authorize(struct vduse_guest_wlan *wlan,
					  const u8 *p, size_t len)
{
	const struct virtio_wlan_authorize *a = (const void *)p;

	if (len != sizeof(*a) || a->authorized > 1)
		return -EINVAL;
	return cfg80211_client_authorize(wlan->client, a->peer, a->authorized);
}

/* Rates that the device definition has no fields for are left out. */
static bool vduse_guest_wlan_rate(const struct rate_info *ri,
				  struct virtio_wlan_rate *r)
{
	static const u8 bws[] = {
		[RATE_INFO_BW_20] = VIRTIO_WLAN_RATE_BW_20,
		[RATE_INFO_BW_5] = VIRTIO_WLAN_RATE_BW_5,
		[RATE_INFO_BW_10] = VIRTIO_WLAN_RATE_BW_10,
		[RATE_INFO_BW_40] = VIRTIO_WLAN_RATE_BW_40,
		[RATE_INFO_BW_80] = VIRTIO_WLAN_RATE_BW_80,
		[RATE_INFO_BW_160] = VIRTIO_WLAN_RATE_BW_160,
		[RATE_INFO_BW_HE_RU] = VIRTIO_WLAN_RATE_BW_HE_RU,
	};

	if (ri->bw >= ARRAY_SIZE(bws) ||
	    (ri->flags & ~(RATE_INFO_FLAGS_MCS | RATE_INFO_FLAGS_VHT_MCS |
			   RATE_INFO_FLAGS_HE_MCS | RATE_INFO_FLAGS_SHORT_GI)))
		return false;

	if (ri->flags & RATE_INFO_FLAGS_HE_MCS)
		r->type = VIRTIO_WLAN_RATE_HE;
	else if (ri->flags & RATE_INFO_FLAGS_VHT_MCS)
		r->type = VIRTIO_WLAN_RATE_VHT;
	else if (ri->flags & RATE_INFO_FLAGS_MCS)
		r->type = VIRTIO_WLAN_RATE_HT;
	else
		r->type = VIRTIO_WLAN_RATE_LEGACY;

	r->bw = bws[ri->bw];
	r->mcs = ri->mcs;
	r->nss = ri->nss;
	if (ri->flags & RATE_INFO_FLAGS_SHORT_GI)
		r->flags |= VIRTIO_WLAN_RATE_F_SHORT_GI;
	r->he_gi = ri->he_gi;
	r->he_dcm = ri->he_dcm;
	r->he_ru_alloc = ri->he_ru_alloc;
	r->legacy = cpu_to_le16(ri->legacy);
	return true;
}

static void vduse_guest_wlan_station(const struct station_info *si,
				     struct virtio_wlan_station_info *out)
{
	u32 filled = 0;

	if (si->filled & BIT_ULL(NL80211_STA_INFO_CONNECTED_TIME)) {
		filled |= VIRTIO_WLAN_STA_CONNECTED_TIME;
		out->connected_time = cpu_to_le32(si->connected_time);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_INACTIVE_TIME)) {
		filled |= VIRTIO_WLAN_STA_INACTIVE_TIME;
		out->inactive_time = cpu_to_le32(si->inactive_time);
	}
	if (si->filled & (BIT_ULL(NL80211_STA_INFO_RX_BYTES) |
			  BIT_ULL(NL80211_STA_INFO_RX_BYTES64))) {
		filled |= VIRTIO_WLAN_STA_RX_BYTES;
		out->rx_bytes = cpu_to_le64(si->rx_bytes);
	}
	if (si->filled & (BIT_ULL(NL80211_STA_INFO_TX_BYTES) |
			  BIT_ULL(NL80211_STA_INFO_TX_BYTES64))) {
		filled |= VIRTIO_WLAN_STA_TX_BYTES;
		out->tx_bytes = cpu_to_le64(si->tx_bytes);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_RX_PACKETS)) {
		filled |= VIRTIO_WLAN_STA_RX_PACKETS;
		out->rx_packets = cpu_to_le32(si->rx_packets);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_TX_PACKETS)) {
		filled |= VIRTIO_WLAN_STA_TX_PACKETS;
		out->tx_packets = cpu_to_le32(si->tx_packets);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_TX_RETRIES)) {
		filled |= VIRTIO_WLAN_STA_TX_RETRIES;
		out->tx_retries = cpu_to_le32(si->tx_retries);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_TX_FAILED)) {
		filled |= VIRTIO_WLAN_STA_TX_FAILED;
		out->tx_failed = cpu_to_le32(si->tx_failed);
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_SIGNAL)) {
		filled |= VIRTIO_WLAN_STA_SIGNAL;
		out->signal = si->signal;
	}
	if (si->filled & BIT_ULL(NL80211_STA_INFO_SIGNAL_AVG)) {
		filled |= VIRTIO_WLAN_STA_SIGNAL_AVG;
		out->signal_avg = si->signal_avg;
	}
	if ((si->filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE)) &&
	    vduse_guest_wlan_rate(&si->txrate, &out->tx_rate))
		filled |= VIRTIO_WLAN_STA_TX_RATE;
	if ((si->filled & BIT_ULL(NL80211_STA_INFO_RX_BITRATE)) &&
	    vduse_guest_wlan_rate(&si->rxrate, &out->rx_rate))
		filled |= VIRTIO_WLAN_STA_RX_RATE;
	out->filled = cpu_to_le32(filled);
}

static int vduse_guest_wlan_cmd_get_station(struct vduse_guest_wlan *wlan,
					    const u8 *p, size_t len,
					    struct virtio_wlan_station_info *out)
{
	const struct virtio_wlan_get_station *g = (const void *)p;
	struct station_info sinfo;
	int err;

	if (len != sizeof(*g))
		return -EINVAL;

	err = cfg80211_client_get_station(wlan->client, g->peer, &sinfo);
	if (err)
		return err;
	vduse_guest_wlan_station(&sinfo, out);
	cfg80211_sinfo_release_content(&sinfo);
	return 0;
}

static int vduse_guest_wlan_cmd_power_mgmt(struct vduse_guest_wlan *wlan,
					   const u8 *p, size_t len)
{
	const struct virtio_wlan_set_power_mgmt *pm = (const void *)p;
	u32 timeout;

	if (len != sizeof(*pm) || pm->enabled > 1)
		return -EINVAL;
	timeout = le32_to_cpu(pm->timeout);
	if (timeout == VIRTIO_WLAN_PS_TIMEOUT_DEFAULT)
		return cfg80211_client_set_power_mgmt(wlan->client, pm->enabled,
						      -1);
	if (timeout > INT_MAX)
		return -EINVAL;
	return cfg80211_client_set_power_mgmt(wlan->client, pm->enabled,
					      timeout);
}

/* The interface must be down, as mac80211 requires for a new address. */
static int vduse_guest_wlan_cmd_set_mac(struct vduse_guest_wlan *wlan,
					const u8 *p, size_t len)
{
	const struct virtio_wlan_set_mac *m = (const void *)p;

	if (!(vduse_guest_driver_features(wlan->dev) &
	      BIT_ULL(VIRTIO_WLAN_F_SET_MAC)))
		return -EOPNOTSUPP;
	if (len != sizeof(*m))
		return -EINVAL;
	return cfg80211_client_set_mac(wlan->client, m->addr);
}

/* A country is two letters, or "00" for the world regulatory domain. */
static int vduse_guest_wlan_cmd_set_country(struct vduse_guest_wlan *wlan,
					    const u8 *p, size_t len)
{
	const struct virtio_wlan_set_country *c = (const void *)p;
	char alpha2[3] = {};

	if (len != sizeof(*c))
		return -EINVAL;
	memcpy(alpha2, c->alpha2, sizeof(c->alpha2));
	if (!(isupper(alpha2[0]) && isupper(alpha2[1])) &&
	    strcmp(alpha2, "00"))
		return -EINVAL;
	return cfg80211_client_set_country(wlan->client, alpha2);
}

/*
 * Run one command. The response's payload is out, and the commands of WPA3
 * and the other P1 and P2 features are not supported.
 */
static int vduse_guest_wlan_dispatch(struct vduse_guest_wlan *wlan, u16 type,
				     const u8 *p, size_t len,
				     const void **out, size_t *out_len,
				     struct virtio_wlan_station_info *sta)
{
	int err;

	switch (type) {
	case VIRTIO_WLAN_CMD_GET_WIPHY:
		if (len)
			return -EINVAL;
		*out = wlan->wiphy_resp;
		*out_len = wlan->wiphy_resp_len;
		return 0;
	case VIRTIO_WLAN_CMD_OPEN:
		if (len)
			return -EINVAL;
		err = cfg80211_client_open(wlan->client);
		/* The host takes its view of the channels from here on. */
		if (!err)
			queue_work(wlan->wq, &wlan->country_work);
		return err;
	case VIRTIO_WLAN_CMD_CLOSE:
		if (len)
			return -EINVAL;
		err = cfg80211_client_close(wlan->client);
		return err == -ENETDOWN ? 0 : err;
	case VIRTIO_WLAN_CMD_SCAN:
		return vduse_guest_wlan_cmd_scan(wlan, p, len);
	case VIRTIO_WLAN_CMD_ABORT_SCAN:
		return vduse_guest_wlan_cmd_abort_scan(wlan, p, len);
	case VIRTIO_WLAN_CMD_CONNECT:
		return vduse_guest_wlan_cmd_connect(wlan, p, len);
	case VIRTIO_WLAN_CMD_DISCONNECT:
		return vduse_guest_wlan_cmd_disconnect(wlan, p, len);
	case VIRTIO_WLAN_CMD_ADD_KEY:
		return vduse_guest_wlan_cmd_add_key(wlan, p, len);
	case VIRTIO_WLAN_CMD_DEL_KEY:
		return vduse_guest_wlan_cmd_del_key(wlan, p, len);
	case VIRTIO_WLAN_CMD_SET_DEFAULT_KEY:
		return vduse_guest_wlan_cmd_default_key(wlan, p, len);
	case VIRTIO_WLAN_CMD_SET_DEFAULT_MGMT_KEY:
		return vduse_guest_wlan_cmd_default_mgmt_key(wlan, p, len);
	case VIRTIO_WLAN_CMD_AUTHORIZE:
		return vduse_guest_wlan_cmd_authorize(wlan, p, len);
	case VIRTIO_WLAN_CMD_GET_STATION:
		err = vduse_guest_wlan_cmd_get_station(wlan, p, len, sta);
		if (!err) {
			*out = sta;
			*out_len = sizeof(*sta);
		}
		return err;
	case VIRTIO_WLAN_CMD_SET_POWER_MGMT:
		return vduse_guest_wlan_cmd_power_mgmt(wlan, p, len);
	case VIRTIO_WLAN_CMD_SET_MAC:
		return vduse_guest_wlan_cmd_set_mac(wlan, p, len);
	case VIRTIO_WLAN_CMD_SET_COUNTRY:
		return vduse_guest_wlan_cmd_set_country(wlan, p, len);
	default:
		return -EOPNOTSUPP;
	}
}

/*
 * Handle a command chain and return the length written into its response.
 * Every length is checked, so that a bad command only fails.
 */
static u32 vduse_guest_wlan_handle(struct vduse_guest_wlan *wlan,
				   struct vringh_kiov *riov,
				   struct vringh_kiov *wiov)
{
	size_t in_len = vringh_kiov_length(riov);
	size_t room = vringh_kiov_length(wiov);
	struct virtio_wlan_station_info sta = {};
	struct virtio_wlan_resp resp = {};
	struct virtio_wlan_cmd_hdr hdr;
	const void *out = NULL;
	size_t out_len = 0;
	struct iov_iter it;
	u8 *req = NULL;
	int err;

	atomic64_inc(&wlan->stats.commands);
	if (room < sizeof(resp)) {
		atomic64_inc(&wlan->stats.invalid_commands);
		return 0;
	}

	if (in_len < sizeof(hdr) || in_len > wlan->cmd_size) {
		err = -EINVAL;
		goto respond;
	}
	req = kmalloc(in_len, GFP_KERNEL);
	if (!req) {
		err = -ENOMEM;
		goto respond;
	}
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, in_len);
	if (copy_from_iter(req, in_len, &it) != in_len) {
		err = -EINVAL;
		goto respond;
	}
	memcpy(&hdr, req, sizeof(hdr));
	if (hdr.flags || le32_to_cpu(hdr.len) != in_len - sizeof(hdr)) {
		err = -EINVAL;
		goto respond;
	}

	err = vduse_guest_wlan_dispatch(wlan, le16_to_cpu(hdr.type),
					req + sizeof(hdr), in_len - sizeof(hdr),
					&out, &out_len, &sta);
	if (!err && sizeof(resp) + out_len > room) {
		err = -EINVAL;
		out_len = 0;
	}

respond:
	if (err)
		out_len = 0;
	if (err == -EINVAL)
		atomic64_inc(&wlan->stats.invalid_commands);
	resp.status = cpu_to_le16(virtio_wlan_errno_to_status(err));
	resp.len = cpu_to_le32(out_len);
	iov_iter_kvec(&it, ITER_DEST, wiov->iov, wiov->used, room);
	kfree_sensitive(req);
	/* The room was checked, so a short copy means the buffer faulted. */
	if (copy_to_iter(&resp, sizeof(resp), &it) != sizeof(resp) ||
	    copy_to_iter(out, out_len, &it) != out_len)
		return 0;
	return sizeof(resp) + out_len;
}

static void vduse_guest_wlan_cmd_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(work, struct vduse_guest_wlan, cmd_work);
	struct kvec rkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct kvec wkvec[VDUSE_GUEST_WLAN_INLINE_IOVS];
	struct vringh_kiov riov, wiov;
	bool running;
	u16 head;
	u32 len;
	int r;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));
	vringh_kiov_init(&wiov, wkvec, ARRAY_SIZE(wkvec));

	for (;;) {
		spin_lock_irq(&wlan->lock);
		running = wlan->running;
		spin_unlock_irq(&wlan->lock);
		if (!running)
			break;

		r = vduse_guest_getdesc(wlan->dev, VIRTIO_WLAN_VQ_CMD, &riov,
					&wiov, &head);
		if (r < 0)
			break;
		if (!r) {
			if (vduse_guest_notify_enable(wlan->dev,
						      VIRTIO_WLAN_VQ_CMD))
				break;
			vduse_guest_notify_disable(wlan->dev, VIRTIO_WLAN_VQ_CMD);
			continue;
		}

		len = vduse_guest_wlan_handle(wlan, &riov, &wiov);
		vduse_guest_complete(wlan->dev, VIRTIO_WLAN_VQ_CMD, head, len);
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

static void vduse_guest_wlan_kick(void *priv, unsigned int index)
{
	struct vduse_guest_wlan *wlan = priv;

	switch (index) {
	case VIRTIO_WLAN_VQ_RX:
		mod_delayed_work(system_percpu_wq, &wlan->rx_work, 0);
		break;
	case VIRTIO_WLAN_VQ_TX:
		set_bit(0, &wlan->tx_kicked);
		queue_work(system_percpu_wq, &wlan->tx_work);
		break;
	case VIRTIO_WLAN_VQ_CMD:
		queue_work(wlan->wq, &wlan->cmd_work);
		break;
	case VIRTIO_WLAN_VQ_EVENT:
		queue_work(wlan->wq, &wlan->event_work);
		break;
	}
}

static int vduse_guest_wlan_start(void *priv)
{
	struct vduse_guest_wlan *wlan = priv;
	struct net_device *netdev = wlan->netdev;
	int err = 0;

	rtnl_lock();
	if (wlan->gone) {
		err = -ENODEV;
	} else {
		err = netdev_rx_handler_register(netdev, vduse_guest_wlan_rx,
						 wlan);
		if (!err) {
			wlan->rx_registered = true;
			netdev_lock_ops(netdev);
			netdev->wanted_features &= ~(NETIF_F_GRO | NETIF_F_LRO);
			netdev_update_features(netdev);
			netdev_unlock_ops(netdev);
		}
	}
	/* The gone notification, which stops the data path, takes the RTNL. */
	if (!err) {
		spin_lock_irq(&wlan->lock);
		wlan->running = true;
		spin_unlock_irq(&wlan->lock);
		spin_lock_bh(&wlan->data_lock);
		wlan->data_running = true;
		spin_unlock_bh(&wlan->data_lock);
	}
	rtnl_unlock();
	return err;
}

/*
 * Leave nothing for the next driver to inherit: end the scan and the
 * connection, which removes every key, close the interface, restore its
 * address, and drop every note and event.
 */
static void vduse_guest_wlan_stop(void *priv)
{
	struct vduse_guest_wlan *wlan = priv;
	struct vduse_guest_wlan_note *note, *ntmp;
	struct vduse_guest_wlan_event *ev, *etmp;
	bool restore_mac = false;
	LIST_HEAD(notes);
	LIST_HEAD(backlog);

	spin_lock_irq(&wlan->lock);
	wlan->running = false;
	spin_unlock_irq(&wlan->lock);
	flush_workqueue(wlan->wq);
	vduse_guest_wlan_stop_data(wlan, false);

	cfg80211_client_abort_scan(wlan->client);
	cfg80211_client_disconnect(wlan->client, WLAN_REASON_DEAUTH_LEAVING);
	cfg80211_client_close(wlan->client);

	rtnl_lock();
	if (!wlan->gone) {
		restore_mac = !ether_addr_equal(wlan->netdev->dev_addr,
						wlan->config.mac);
		if (wlan->rx_registered) {
			netdev_rx_handler_unregister(wlan->netdev);
			wlan->rx_registered = false;
		}
	}
	rtnl_unlock();
	if (restore_mac)
		cfg80211_client_set_mac(wlan->client, wlan->config.mac);

	spin_lock_irq(&wlan->lock);
	list_splice_init(&wlan->notes, &notes);
	list_splice_init(&wlan->backlog, &backlog);
	wlan->backlog_len = 0;
	wlan->scan_active = false;
	wlan->conn_active = false;
	spin_unlock_irq(&wlan->lock);

	list_for_each_entry_safe(note, ntmp, &notes, list)
		vduse_guest_wlan_free_note(wlan, note);
	list_for_each_entry_safe(ev, etmp, &backlog, list)
		kfree(ev);
}

static const struct {
	u32 kernel;
	u32 wire;
} vduse_guest_wlan_chan_flags[] = {
	{ IEEE80211_CHAN_DISABLED, VIRTIO_WLAN_CHAN_DISABLED },
	{ IEEE80211_CHAN_NO_IR, VIRTIO_WLAN_CHAN_NO_IR },
	{ IEEE80211_CHAN_RADAR, VIRTIO_WLAN_CHAN_RADAR },
	{ IEEE80211_CHAN_NO_HT40PLUS, VIRTIO_WLAN_CHAN_NO_HT40PLUS },
	{ IEEE80211_CHAN_NO_HT40MINUS, VIRTIO_WLAN_CHAN_NO_HT40MINUS },
	{ IEEE80211_CHAN_NO_OFDM, VIRTIO_WLAN_CHAN_NO_OFDM },
	{ IEEE80211_CHAN_NO_80MHZ, VIRTIO_WLAN_CHAN_NO_80MHZ },
	{ IEEE80211_CHAN_NO_160MHZ, VIRTIO_WLAN_CHAN_NO_160MHZ },
	{ IEEE80211_CHAN_INDOOR_ONLY, VIRTIO_WLAN_CHAN_INDOOR_ONLY },
	{ IEEE80211_CHAN_IR_CONCURRENT, VIRTIO_WLAN_CHAN_IR_CONCURRENT },
	{ IEEE80211_CHAN_NO_20MHZ, VIRTIO_WLAN_CHAN_NO_20MHZ },
	{ IEEE80211_CHAN_NO_10MHZ, VIRTIO_WLAN_CHAN_NO_10MHZ },
	{ IEEE80211_CHAN_NO_HE, VIRTIO_WLAN_CHAN_NO_HE },
};

/* Describe a channel with the given flags and maximum power. */
static void vduse_guest_wlan_channel(const struct ieee80211_channel *chan,
				     u32 kflags, int power,
				     struct virtio_wlan_channel *out)
{
	u32 flags = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(vduse_guest_wlan_chan_flags); i++)
		if (kflags & vduse_guest_wlan_chan_flags[i].kernel)
			flags |= vduse_guest_wlan_chan_flags[i].wire;

	out->freq = cpu_to_le16(chan->center_freq);
	out->max_power = cpu_to_le16(clamp_t(int, power, 0, U16_MAX));
	out->flags = cpu_to_le32(flags);
}

static void vduse_guest_wlan_band(const struct ieee80211_supported_band *sband,
				  u8 id, struct virtio_wlan_band *out)
{
	const struct ieee80211_sband_iftype_data *data;
	struct virtio_wlan_channel *chans = (void *)(out + 1);
	int i;

	out->band = id;
	out->n_channels = cpu_to_le16(sband->n_channels);

	if (sband->ht_cap.ht_supported) {
		out->flags |= VIRTIO_WLAN_BAND_F_HT;
		out->ht.cap = cpu_to_le16(sband->ht_cap.cap);
		out->ht.ampdu_factor = sband->ht_cap.ampdu_factor;
		out->ht.ampdu_density = sband->ht_cap.ampdu_density;
		memcpy(out->ht.rx_mask, sband->ht_cap.mcs.rx_mask,
		       sizeof(out->ht.rx_mask));
		out->ht.rx_highest = sband->ht_cap.mcs.rx_highest;
		out->ht.tx_params = sband->ht_cap.mcs.tx_params;
	}

	if (sband->vht_cap.vht_supported) {
		out->flags |= VIRTIO_WLAN_BAND_F_VHT;
		out->vht.cap = cpu_to_le32(sband->vht_cap.cap);
		out->vht.rx_mcs_map = sband->vht_cap.vht_mcs.rx_mcs_map;
		out->vht.rx_highest = sband->vht_cap.vht_mcs.rx_highest;
		out->vht.tx_mcs_map = sband->vht_cap.vht_mcs.tx_mcs_map;
		out->vht.tx_highest = sband->vht_cap.vht_mcs.tx_highest;
	}

	data = ieee80211_get_sband_iftype_data(sband, NL80211_IFTYPE_STATION);
	if (data && data->he_cap.has_he) {
		const struct ieee80211_sta_he_cap *he = &data->he_cap;

		out->flags |= VIRTIO_WLAN_BAND_F_HE;
		memcpy(out->he.mac_cap, he->he_cap_elem.mac_cap_info,
		       sizeof(out->he.mac_cap));
		memcpy(out->he.phy_cap, he->he_cap_elem.phy_cap_info,
		       sizeof(out->he.phy_cap));
		out->he.rx_mcs_80 = he->he_mcs_nss_supp.rx_mcs_80;
		out->he.tx_mcs_80 = he->he_mcs_nss_supp.tx_mcs_80;
		out->he.rx_mcs_160 = he->he_mcs_nss_supp.rx_mcs_160;
		out->he.tx_mcs_160 = he->he_mcs_nss_supp.tx_mcs_160;
		out->he.rx_mcs_80p80 = he->he_mcs_nss_supp.rx_mcs_80p80;
		out->he.tx_mcs_80p80 = he->he_mcs_nss_supp.tx_mcs_80p80;
		memcpy(out->he.ppe_thres, he->ppe_thres,
		       sizeof(out->he.ppe_thres));
		if (id == VIRTIO_WLAN_BAND_6GHZ) {
			out->flags |= VIRTIO_WLAN_BAND_F_HE_6GHZ;
			out->he_6ghz_capa = data->he_6ghz_capa.capa;
		}
	}

	for (i = 0; i < sband->n_channels; i++)
		vduse_guest_wlan_channel(&sband->channels[i],
					 sband->channels[i].orig_flags,
					 sband->channels[i].orig_mpwr, &chans[i]);
}

/*
 * Report the country that applies to the interface and the channels as it
 * leaves them. The driver can free its channels once the interface is gone,
 * so they are read under the RTNL.
 */
static void vduse_guest_wlan_country_work(struct work_struct *work)
{
	struct vduse_guest_wlan *wlan =
		container_of(work, struct vduse_guest_wlan, country_work);
	static const enum nl80211_band bands[] = {
		NL80211_BAND_2GHZ, NL80211_BAND_5GHZ, NL80211_BAND_6GHZ,
	};
	struct virtio_wlan_event_country *e;
	struct ieee80211_supported_band *sband;
	struct vduse_guest_wlan_event *ev = NULL;
	struct virtio_wlan_channel *chans;
	unsigned int n = 0;
	char alpha2[2];
	int b, i;

	rtnl_lock();
	if (wlan->gone ||
	    cfg80211_client_get_country(wlan->client, alpha2))
		goto out;

	for (b = 0; b < ARRAY_SIZE(bands); b++)
		if (wlan->wiphy->bands[bands[b]])
			n += wlan->wiphy->bands[bands[b]]->n_channels;
	if (sizeof(struct virtio_wlan_event_hdr) + sizeof(*e) +
	    n * sizeof(*chans) > VDUSE_GUEST_WLAN_EVENT_SIZE)
		goto out;
	ev = vduse_guest_wlan_event_alloc(VIRTIO_WLAN_EVENT_COUNTRY,
					  sizeof(*e) + n * sizeof(*chans),
					  GFP_KERNEL);
	if (!ev)
		goto out;

	e = vduse_guest_wlan_event_payload(ev);
	memcpy(e->alpha2, alpha2, sizeof(e->alpha2));
	e->n_channels = cpu_to_le16(n);
	chans = (void *)(e + 1);
	for (b = 0; b < ARRAY_SIZE(bands); b++) {
		sband = wlan->wiphy->bands[bands[b]];
		for (i = 0; sband && i < sband->n_channels; i++)
			vduse_guest_wlan_channel(&sband->channels[i],
						 sband->channels[i].flags,
						 sband->channels[i].max_power,
						 chans++);
	}
out:
	rtnl_unlock();
	if (!ev)
		return;
	vduse_guest_wlan_queue_event(wlan, ev, false);
	queue_work(wlan->wq, &wlan->event_work);
}

/* Describe the wiphy as GET_WIPHY reports it, for the 2.4, 5 and 6 GHz bands. */
static int vduse_guest_wlan_build_wiphy(struct vduse_guest_wlan *wlan)
{
	static const struct {
		enum nl80211_band band;
		u8 id;
	} bands[] = {
		{ NL80211_BAND_2GHZ, VIRTIO_WLAN_BAND_2GHZ },
		{ NL80211_BAND_5GHZ, VIRTIO_WLAN_BAND_5GHZ },
		{ NL80211_BAND_6GHZ, VIRTIO_WLAN_BAND_6GHZ },
	};
	struct wiphy *wiphy = wlan->wiphy;
	unsigned int n_channels = 0;
	struct virtio_wlan_wiphy *w;
	size_t size = sizeof(*w);
	u32 flags = 0;
	u8 *p;
	int i;

	for (i = 0; i < ARRAY_SIZE(bands); i++) {
		if (!wiphy->bands[bands[i].band])
			continue;
		n_channels += wiphy->bands[bands[i].band]->n_channels;
		size += sizeof(struct virtio_wlan_band) +
			wiphy->bands[bands[i].band]->n_channels *
			sizeof(struct virtio_wlan_channel);
	}
	if (size > VDUSE_GUEST_WLAN_WIPHY_SIZE)
		return -E2BIG;

	/*
	 * The largest command is a scan of every channel with the most SSIDs
	 * and IEs, or a connection.
	 */
	wlan->cmd_size = sizeof(struct virtio_wlan_cmd_hdr) +
			 max(sizeof(struct virtio_wlan_scan) +
			     wiphy->max_scan_ssids *
			     sizeof(struct virtio_wlan_ssid) +
			     n_channels * sizeof(__le16) +
			     wiphy->max_scan_ie_len,
			     sizeof(struct virtio_wlan_connect) +
			     IEEE80211_MAX_DATA_LEN);

	w = kzalloc(size, GFP_KERNEL);
	if (!w)
		return -ENOMEM;

	if (wiphy->features & NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR)
		flags |= VIRTIO_WLAN_WIPHY_F_SCAN_RANDOM_MAC;
	if (wiphy_ext_feature_isset(wiphy, NL80211_EXT_FEATURE_MFP_OPTIONAL))
		flags |= VIRTIO_WLAN_WIPHY_F_MFP_OPTIONAL;
	if (wiphy_ext_feature_isset(wiphy, NL80211_EXT_FEATURE_CAN_REPLACE_PTK0))
		flags |= VIRTIO_WLAN_WIPHY_F_CAN_REPLACE_PTK0;
	w->flags = cpu_to_le32(flags);
	w->max_scan_ie_len = cpu_to_le16(wiphy->max_scan_ie_len);
	w->max_scan_ssids = wiphy->max_scan_ssids;
	w->max_akm_suites = wiphy->max_num_akm_suites;
	w->n_cipher_suites = min_t(int, wiphy->n_cipher_suites,
				   VIRTIO_WLAN_MAX_CIPHER_SUITES);
	for (i = 0; i < w->n_cipher_suites; i++)
		w->cipher_suites[i] = cpu_to_le32(wiphy->cipher_suites[i]);

	p = (u8 *)(w + 1);
	for (i = 0; i < ARRAY_SIZE(bands); i++) {
		struct ieee80211_supported_band *sband =
			wiphy->bands[bands[i].band];

		if (!sband)
			continue;
		vduse_guest_wlan_band(sband, bands[i].id, (void *)p);
		p += sizeof(struct virtio_wlan_band) +
		     sband->n_channels * sizeof(struct virtio_wlan_channel);
		w->n_bands++;
	}

	wlan->wiphy_resp = w;
	wlan->wiphy_resp_len = size;
	return 0;
}

/* Return a held station interface, the named one or else the first. */
static struct net_device *vduse_guest_wlan_find_netdev(const char *name)
{
	struct net_device *dev, *found = NULL;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (!dev->ieee80211_ptr ||
		    dev->ieee80211_ptr->iftype != NL80211_IFTYPE_STATION)
			continue;
		if (name && strcmp(dev->name, name))
			continue;
		found = dev;
		dev_hold(found);
		break;
	}
	rtnl_unlock();
	return found;
}

struct vduse_guest_wlan_wait {
	struct notifier_block nb;
	struct completion registered;
};

static int vduse_guest_wlan_netdev_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct vduse_guest_wlan_wait *w =
		container_of(nb, struct vduse_guest_wlan_wait, nb);

	if (event == NETDEV_REGISTER)
		complete(&w->registered);
	return NOTIFY_DONE;
}

/*
 * Become the client of the interface. A driver can register its interface
 * from a work item after its firmware loads, so this waits for one to
 * appear, up to the timeout.
 */
static int vduse_guest_wlan_attach(struct vduse_guest_wlan *wlan,
				   const char *name, unsigned int timeout)
{
	const char *dev_name = vduse_guest_name(wlan->dev);
	unsigned long deadline = jiffies + timeout * HZ;
	struct vduse_guest_wlan_wait w = {
		.nb.notifier_call = vduse_guest_wlan_netdev_event,
	};
	struct cfg80211_client *client;
	struct net_device *dev;
	int err;

	init_completion(&w.registered);
	wait_for_device_probe();
	err = register_netdevice_notifier(&w.nb);
	if (err)
		return err;

	for (;;) {
		dev = vduse_guest_wlan_find_netdev(name);
		if (dev) {
			/* The gone notification can run before the call returns. */
			wlan->netdev = dev;
			wlan->wiphy = dev->ieee80211_ptr->wiphy;
			client = cfg80211_client_register(dev,
					&vduse_guest_wlan_client_ops, wlan);
			if (!IS_ERR(client)) {
				wlan->client = client;
				dev_put(dev);
				break;
			}
			dev_put(dev);
			err = PTR_ERR(client);
			if (err != -ENODEV)
				goto out;
		}
		if (time_after(jiffies, deadline)) {
			pr_err("%s: no station interface%s%s appeared in %u seconds\n",
			       dev_name, name ? " " : "", name ?: "", timeout);
			err = -ENODEV;
			goto out;
		}
		wait_for_completion_timeout(&w.registered, HZ / 10);
		reinit_completion(&w.registered);
	}
	err = 0;
out:
	unregister_netdevice_notifier(&w.nb);
	return err;
}

static void *__init vduse_guest_wlan_setup(struct vduse_guest_dev *dev,
					   char *opts,
					   struct vduse_guest_config *cfg)
{
	const char *name = vduse_guest_name(dev);
	unsigned int timeout = VDUSE_GUEST_WLAN_TIMEOUT;
	struct vduse_guest_wlan *wlan;
	char *netdev = NULL, *opt;
	char ifname[IFNAMSIZ];
	int err;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "netdev=")) {
			netdev = opt + strlen("netdev=");
		} else if (strstarts(opt, "timeout=")) {
			if (kstrtouint(opt + strlen("timeout="), 10, &timeout))
				return ERR_PTR(-EINVAL);
		} else if (*opt) {
			return ERR_PTR(-EINVAL);
		}
	}

	wlan = kzalloc_obj(*wlan);
	if (!wlan)
		return ERR_PTR(-ENOMEM);
	wlan->dev = dev;
	spin_lock_init(&wlan->lock);
	INIT_LIST_HEAD(&wlan->notes);
	INIT_LIST_HEAD(&wlan->backlog);
	INIT_WORK(&wlan->cmd_work, vduse_guest_wlan_cmd_work);
	INIT_WORK(&wlan->note_work, vduse_guest_wlan_note_work);
	INIT_WORK(&wlan->event_work, vduse_guest_wlan_event_work);
	INIT_WORK(&wlan->country_work, vduse_guest_wlan_country_work);
	INIT_DELAYED_WORK(&wlan->rx_work, vduse_guest_wlan_rx_work);
	INIT_WORK(&wlan->tx_work, vduse_guest_wlan_tx_work);
	spin_lock_init(&wlan->data_lock);
	INIT_LIST_HEAD(&wlan->rx_bufs);
	wlan->control_port = htons(ETH_P_PAE);

	wlan->wq = alloc_ordered_workqueue("vduse-wlan-%s", 0, name);
	if (!wlan->wq) {
		kfree(wlan);
		return ERR_PTR(-ENOMEM);
	}

	err = vduse_guest_wlan_attach(wlan, netdev, timeout);
	if (err)
		goto free;

	/* The driver can free its channel arrays once the interface is gone. */
	rtnl_lock();
	if (wlan->gone) {
		err = -ENODEV;
		goto unlock;
	}
	err = vduse_guest_wlan_build_wiphy(wlan);
	if (err)
		goto unlock;
	ether_addr_copy(wlan->config.mac, wlan->netdev->dev_addr);
	wlan->config.mtu = cpu_to_le16(wlan->netdev->mtu);
	strscpy(ifname, wlan->netdev->name);
	rtnl_unlock();
	wlan->config.max_event_size = cpu_to_le32(VDUSE_GUEST_WLAN_EVENT_SIZE);
	wlan->config.max_wiphy_size = cpu_to_le32(VDUSE_GUEST_WLAN_WIPHY_SIZE);

	cfg->device_id = VIRTIO_ID_MAC80211_WLAN;
	cfg->features = BIT_ULL(VIRTIO_WLAN_F_SET_MAC);
	cfg->num_queues = VIRTIO_WLAN_VQ_MAX;
	cfg->queue_sizes = vduse_guest_wlan_queue_sizes;
	/*
	 * A new guest could not recover the host's view of the interface,
	 * such as whether OPEN was sent.
	 */
	cfg->allow_reconnect = false;
	cfg->config = &wlan->config;
	cfg->config_size = sizeof(wlan->config);

	pr_info("%s: serving %s with %zu bytes of wiphy description\n", name,
		ifname, wlan->wiphy_resp_len);
	return wlan;

unlock:
	wlan->gone = true;
	rtnl_unlock();
	kfree(wlan->wiphy_resp);
	cfg80211_client_unregister(wlan->client);
free:
	destroy_workqueue(wlan->wq);
	kfree(wlan);
	return ERR_PTR(err);
}

static void vduse_guest_wlan_release(void *priv)
{
	struct vduse_guest_wlan *wlan = priv;

	/* A country work that runs after this does not use the client. */
	rtnl_lock();
	wlan->gone = true;
	rtnl_unlock();
	cfg80211_client_unregister(wlan->client);
	destroy_workqueue(wlan->wq);
	kfree(wlan->wiphy_resp);
	kfree(wlan);
}

static void vduse_guest_wlan_show(void *priv, struct seq_file *m)
{
	struct vduse_guest_wlan *wlan = priv;
	struct vduse_guest_wlan_stats *s = &wlan->stats;

	seq_printf(m, "  %-18s %lld\n", "commands", atomic64_read(&s->commands));
	seq_printf(m, "  %-18s %lld\n", "invalid_commands",
		   atomic64_read(&s->invalid_commands));
	seq_printf(m, "  %-18s %lld\n", "events", atomic64_read(&s->events));
	seq_printf(m, "  %-18s %lld\n", "bss_dropped",
		   atomic64_read(&s->bss_dropped));
	seq_printf(m, "  %-18s %lld\n", "events_dropped",
		   atomic64_read(&s->events_dropped));
	seq_printf(m, "  %-18s %lld\n", "tx_frames",
		   atomic64_read(&s->tx_frames));
	seq_printf(m, "  %-18s %lld\n", "tx_invalid",
		   atomic64_read(&s->tx_invalid));
	seq_printf(m, "  %-18s %lld\n", "tx_spoofed",
		   atomic64_read(&s->tx_spoofed));
	seq_printf(m, "  %-18s %lld\n", "tx_dropped",
		   atomic64_read(&s->tx_dropped));
	seq_printf(m, "  %-18s %lld\n", "tx_chained",
		   atomic64_read(&s->tx_chained));
	seq_printf(m, "  %-18s %d\n", "tx_pending",
		   atomic_read(&wlan->tx_pending));
	seq_printf(m, "  %-18s %lld\n", "rx_frames",
		   atomic64_read(&s->rx_frames));
	seq_printf(m, "  %-18s %lld\n", "rx_no_buffer",
		   atomic64_read(&s->rx_no_buffer));
	seq_printf(m, "  %-18s %lld\n", "rx_oversized",
		   atomic64_read(&s->rx_oversized));
	seq_printf(m, "  %-18s %lld\n", "rx_bad_buffers",
		   atomic64_read(&s->rx_bad_buffers));
}

const struct vduse_guest_backend_ops vduse_guest_wlan_ops = {
	.type = "wlan",
	.setup = vduse_guest_wlan_setup,
	.release = vduse_guest_wlan_release,
	.start = vduse_guest_wlan_start,
	.stop = vduse_guest_wlan_stop,
	.kick = vduse_guest_wlan_kick,
	.show = vduse_guest_wlan_show,
};
