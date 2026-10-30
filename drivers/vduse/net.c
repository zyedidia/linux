// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-guest-net: " fmt

#include <linux/completion.h>
#include <linux/device/driver.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>
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
#include <linux/virtio_byteorder.h>
#include <linux/virtio_net.h>
#include <linux/workqueue.h>
#include <net/gso.h>
#include <net/netdev_lock.h>
#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_net.h>

#define VDUSE_GUEST_NET_RX 0
#define VDUSE_GUEST_NET_TX 1
#define VDUSE_GUEST_NET_NUM_QUEUES 2

#define VDUSE_GUEST_NET_TIMEOUT 30
#define VDUSE_GUEST_NET_TX_LIMIT 256
#define VDUSE_GUEST_NET_INLINE_IOVS 4
/* The most buffers that one received packet takes. */
#define VDUSE_GUEST_NET_MAX_BUFS 64
/* The linear part of a transmitted frame whose header length is unknown. */
#define VDUSE_GUEST_NET_GOODCOPY 128
#define VDUSE_GUEST_NET_LINK_RETRY (HZ / 10)

#define VDUSE_GUEST_NET_FEATURES					\
	(BIT_ULL(VIRTIO_NET_F_MAC) | BIT_ULL(VIRTIO_NET_F_MTU) |	\
	 BIT_ULL(VIRTIO_NET_F_STATUS) |					\
	 BIT_ULL(VIRTIO_NET_F_SPEED_DUPLEX) |				\
	 BIT_ULL(VIRTIO_NET_F_MRG_RXBUF) | BIT_ULL(VIRTIO_NET_F_CSUM) |	\
	 BIT_ULL(VIRTIO_NET_F_HOST_TSO4) |				\
	 BIT_ULL(VIRTIO_NET_F_HOST_TSO6) |				\
	 BIT_ULL(VIRTIO_NET_F_HOST_ECN) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_CSUM) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_TSO4) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_TSO6) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_ECN))

/* The receive offloads that GRO in the guest needs from the host. */
#define VDUSE_GUEST_NET_GRO_FEATURES					\
	(BIT_ULL(VIRTIO_NET_F_GUEST_CSUM) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_TSO4) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_TSO6) |				\
	 BIT_ULL(VIRTIO_NET_F_GUEST_ECN))

/*
 * The GSO types that a received skb can carry to the host. Others, such as
 * a tunnel's or TCP_ACCECN, which keeps CWR on every segment, are split
 * into their segments first.
 */
#define VDUSE_GUEST_NET_RX_GSO						\
	(SKB_GSO_TCPV4 | SKB_GSO_TCPV6 | SKB_GSO_TCP_ECN |		\
	 SKB_GSO_TCP_FIXEDID | SKB_GSO_DODGY)

#define VDUSE_GUEST_NET_CONFIG_SIZE \
	offsetofend(struct virtio_net_config, duplex)

#define VDUSE_GUEST_NET_RX_SIZE 1024

static const u16 vduse_guest_net_queue_sizes[VDUSE_GUEST_NET_NUM_QUEUES] = {
	[VDUSE_GUEST_NET_RX] = VDUSE_GUEST_NET_RX_SIZE,
	[VDUSE_GUEST_NET_TX] = 256,
};

/*
 * Tx_invalid counts the host's frames with a bad length or header, and
 * tx_dropped those that found no memory. Rx_no_buffer counts the received
 * frames that found too few buffers from the host, rx_oversized those that
 * did not fit in the most buffers that a packet may take, and
 * rx_unsupported those with an offload that the host did not accept.
 * Rx_segmented counts the received aggregates split into their segments.
 * Rx_dropped counts the received frames that found no memory to copy, to
 * put back their VLAN tag, or to finish the checksum or the segmentation
 * that the host needed.
 */
struct vduse_guest_net_stats {
	atomic64_t tx_frames;
	atomic64_t tx_gso;
	atomic64_t tx_invalid;
	atomic64_t tx_dropped;
	atomic64_t rx_frames;
	atomic64_t rx_gso;
	atomic64_t rx_buffers;
	atomic64_t rx_no_buffer;
	atomic64_t rx_oversized;
	atomic64_t rx_unsupported;
	atomic64_t rx_segmented;
	atomic64_t rx_dropped;
	atomic64_t link_changes;
};

/* A buffer that the host posted on the rx queue, waiting for a frame. */
struct vduse_guest_net_rx_buf {
	struct list_head list;
	u16 head;
	struct vringh_kiov wiov;
	struct kvec kvec[VDUSE_GUEST_NET_INLINE_IOVS];
};

struct vduse_guest_net {
	struct vduse_guest_dev *dev;
	struct net_device *netdev;
	netdevice_tracker tracker;
	struct notifier_block nb;
	/* The configuration space given when the device is created. */
	struct virtio_net_config create_config;

	/*
	 * Set under the RTNL. Gone is set once the interface is unregistered
	 * or the backend lets it go, after which the notifier ignores it.
	 * Opened is set once the open has changed the interface, and was_up
	 * and saved_mtu record what to restore. Running is set from start to
	 * stop, and config_dirty while the host has not been told of a change
	 * to config.
	 */
	bool gone;
	bool opened;
	bool was_up;
	unsigned int saved_mtu;
	char ifname[IFNAMSIZ];
	bool rx_registered;
	bool running;
	bool config_dirty;
	struct virtio_net_config config;
	struct delayed_work link_work;

	/*
	 * The rx and tx works run where their queues' kicks are handled, and
	 * the tx work also where a destructor restarts it. The rx handler runs
	 * where the interface receives. Data_lock protects data_running,
	 * features and the rx buffers, which come from a pool with one for
	 * each entry of the rx queue, so that taking the host's buffers needs
	 * no allocation. Features holds the ones that the host accepted, while
	 * the data path runs. Tx_pending counts the frames in the qdisc and the
	 * NIC, apart from those segmented in software, which stop counting once
	 * they are split. The tx work stops at the limit until the destructor
	 * of one of them sets tx_wake.
	 */
	struct work_struct rx_work;
	struct work_struct tx_work;
	spinlock_t data_lock;
	bool data_running;
	u64 features;
	struct list_head rx_bufs;
	struct list_head rx_free;
	struct vduse_guest_net_rx_buf *rx_pool;
	atomic_t tx_pending;
	unsigned long tx_wake;
	/*
	 * The CPU of the last tx kick, where the destructor restarts the tx
	 * work too. Frames then reach the interface from one CPU, so that
	 * transmit steering by CPU keeps a flow on one NIC queue.
	 */
	int tx_cpu;

	struct vduse_guest_net_stats stats;
};

static bool vduse_guest_net_data_running(struct vduse_guest_net *net,
					 u64 *features)
{
	bool running;

	spin_lock_bh(&net->data_lock);
	running = net->data_running;
	*features = net->features;
	spin_unlock_bh(&net->data_lock);
	return running;
}

static void vduse_guest_net_queue_tx(struct vduse_guest_net *net)
{
	int cpu = READ_ONCE(net->tx_cpu);

	if (cpu != WORK_CPU_UNBOUND && cpu_online(cpu))
		queue_work_on(cpu, system_percpu_wq, &net->tx_work);
	else
		queue_work(system_percpu_wq, &net->tx_work);
}

static void vduse_guest_net_tx_destructor(struct sk_buff *skb)
{
	struct vduse_guest_net *net = skb_shinfo(skb)->destructor_arg;

	atomic_dec(&net->tx_pending);
	/* This pairs with the barrier in vduse_guest_net_tx_full(). */
	smp_mb__after_atomic();
	if (test_and_clear_bit(0, &net->tx_wake))
		vduse_guest_net_queue_tx(net);
}

/* Check a GSO header from the host against the features it accepted. */
static bool vduse_guest_net_tx_gso_ok(const struct virtio_net_hdr_v1 *hdr,
				      u64 features)
{
	u8 type = hdr->gso_type & ~VIRTIO_NET_HDR_GSO_ECN;

	if ((hdr->gso_type & VIRTIO_NET_HDR_GSO_ECN) &&
	    !(features & BIT_ULL(VIRTIO_NET_F_HOST_ECN)))
		return false;
	if (type == VIRTIO_NET_HDR_GSO_TCPV4)
		return features & BIT_ULL(VIRTIO_NET_F_HOST_TSO4);
	if (type == VIRTIO_NET_HDR_GSO_TCPV6)
		return features & BIT_ULL(VIRTIO_NET_F_HOST_TSO6);
	return false;
}

/*
 * Send a frame from the host. The headers go in the linear part of the skb
 * and the rest in page fragments, so that a large segment needs no
 * high-order allocation. The interface segments and checksums what the
 * host offloads, or validate_xmit_skb() does it in software.
 */
static void vduse_guest_net_tx_one(struct vduse_guest_net *net,
				   struct vringh_kiov *riov, u64 features)
{
	struct net_device *netdev = net->netdev;
	size_t len = vringh_kiov_length(riov);
	unsigned int reserve = LL_RESERVED_SPACE(netdev);
	struct virtio_net_hdr_v1 hdr;
	size_t frame_len, max_len, linear;

	struct sk_buff *skb;
	struct iov_iter it;
	const u16 *sap;
	int depth, err;
	u16 sap_buf;

	if (len < sizeof(hdr) + ETH_HLEN)
		goto invalid;
	frame_len = len - sizeof(hdr);
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, len);
	if (copy_from_iter(&hdr, sizeof(hdr), &it) != sizeof(hdr))
		goto invalid;

	if ((hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM) &&
	    !(features & BIT_ULL(VIRTIO_NET_F_CSUM)))
		goto invalid;
	if (hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE) {
		if (!vduse_guest_net_tx_gso_ok(&hdr, features))
			goto invalid;
		max_len = ETH_HLEN + VLAN_MAX_DEPTH * VLAN_HLEN +
			  GSO_LEGACY_MAX_SIZE;
		linear = __virtio16_to_cpu(true, hdr.hdr_len);
	} else {
		max_len = ETH_HLEN + VLAN_MAX_DEPTH * VLAN_HLEN +
			  READ_ONCE(netdev->mtu);
		linear = VDUSE_GUEST_NET_GOODCOPY;
	}
	if (frame_len > max_len)
		goto invalid;
	linear = clamp_t(size_t, linear, ETH_HLEN, SKB_MAX_HEAD(reserve));
	linear = min(linear, frame_len);

	skb = alloc_skb_with_frags(reserve + linear, frame_len - linear,
				   PAGE_ALLOC_COSTLY_ORDER, &err, GFP_KERNEL);
	if (!skb) {
		atomic64_inc(&net->stats.tx_dropped);
		return;
	}
	/*
	 * Probing the transport header dissects the frame, which finds the
	 * network namespace through the device.
	 */
	skb->dev = netdev;
	skb_reserve(skb, reserve);
	skb_put(skb, linear);
	skb->data_len = frame_len - linear;
	skb->len += frame_len - linear;
	if (skb_copy_datagram_from_iter(skb, 0, &it, frame_len))
		goto free;

	skb_reset_mac_header(skb);
	skb_set_network_header(skb, ETH_HLEN);
	/* A length in place of a type is mapped as eth_type_trans() does. */
	skb->protocol = eth_hdr(skb)->h_proto;
	if (!eth_proto_is_802_3(skb->protocol)) {
		sap = skb_header_pointer(skb, ETH_HLEN, sizeof(sap_buf), &sap_buf);
		skb->protocol = sap && *sap == 0xFFFF ? htons(ETH_P_802_3) :
							htons(ETH_P_802_2);
	}
	if (virtio_net_hdr_to_skb(skb, (struct virtio_net_hdr *)&hdr, true))
		goto free;
	skb_probe_transport_header(skb);
	if (eth_type_vlan(skb->protocol) &&
	    vlan_get_protocol_and_depth(skb, skb->protocol, &depth) != 0)
		skb_set_network_header(skb, depth);

	if (skb_is_gso(skb))
		atomic64_inc(&net->stats.tx_gso);
	skb_shinfo(skb)->destructor_arg = net;
	skb->destructor = vduse_guest_net_tx_destructor;
	atomic_inc(&net->tx_pending);
	atomic64_inc(&net->stats.tx_frames);
	dev_queue_xmit(skb);
	return;

free:
	kfree_skb(skb);
invalid:
	atomic64_inc(&net->stats.tx_invalid);
}

/*
 * Stop at the limit of frames in the interface, with notifications
 * disabled, so that the destructor that frees room restarts the work.
 */
static bool vduse_guest_net_tx_full(struct vduse_guest_net *net)
{
	if (atomic_read(&net->tx_pending) < VDUSE_GUEST_NET_TX_LIMIT)
		return false;
	set_bit(0, &net->tx_wake);
	/*
	 * A destructor that ran before the flag was set is seen in the count
	 * read below, and one after it sees the flag.
	 */
	smp_mb__after_atomic();
	if (atomic_read(&net->tx_pending) < VDUSE_GUEST_NET_TX_LIMIT &&
	    test_and_clear_bit(0, &net->tx_wake))
		return false;
	return true;
}

static void vduse_guest_net_tx_work(struct work_struct *work)
{
	struct vduse_guest_net *net =
		container_of(work, struct vduse_guest_net, tx_work);
	struct kvec rkvec[VDUSE_GUEST_NET_INLINE_IOVS];
	struct kvec wkvec[VDUSE_GUEST_NET_INLINE_IOVS];
	struct vringh_kiov riov, wiov;
	u64 features;
	u16 head;
	int r;

	/*
	 * A destructor can queue the work after the device stopped, when the
	 * rings may be gone.
	 */
	if (!vduse_guest_net_data_running(net, &features))
		return;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));
	vringh_kiov_init(&wiov, wkvec, ARRAY_SIZE(wkvec));

	vduse_guest_notify_disable(net->dev, VDUSE_GUEST_NET_TX);
	/*
	 * Under sustained traffic the work yields on each pass, so that it
	 * does not keep other tasks from the CPU. Other work on the
	 * workqueue waits until the workqueue counts this work as CPU
	 * intensive.
	 */
	for (;; cond_resched()) {
		if (!vduse_guest_net_data_running(net, &features) ||
		    vduse_guest_net_tx_full(net))
			break;

		r = vduse_guest_getdesc(net->dev, VDUSE_GUEST_NET_TX, &riov,
					&wiov, &head);
		if (r < 0)
			break;
		if (!r) {
			if (vduse_guest_poll(net->dev, VDUSE_GUEST_NET_TX))
				continue;
			if (vduse_guest_notify_enable(net->dev,
						      VDUSE_GUEST_NET_TX))
				break;
			vduse_guest_notify_disable(net->dev, VDUSE_GUEST_NET_TX);
			continue;
		}

		vduse_guest_net_tx_one(net, &riov, features);
		vduse_guest_complete(net->dev, VDUSE_GUEST_NET_TX, head, 0);
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

static struct vduse_guest_net_rx_buf *
vduse_guest_net_get_rx_buf(struct vduse_guest_net *net)
{
	struct vduse_guest_net_rx_buf *buf;

	spin_lock_bh(&net->data_lock);
	buf = list_first_entry_or_null(&net->rx_free,
				       struct vduse_guest_net_rx_buf, list);
	if (buf)
		list_del(&buf->list);
	spin_unlock_bh(&net->data_lock);
	return buf;
}

/* Return buffers to the pool, with any iovecs that vringh added freed. */
static void vduse_guest_net_put_rx_bufs(struct vduse_guest_net *net,
					struct list_head *bufs)
{
	struct vduse_guest_net_rx_buf *buf;

	list_for_each_entry(buf, bufs, list) {
		vringh_kiov_cleanup(&buf->wiov);
		vringh_kiov_init(&buf->wiov, buf->kvec, ARRAY_SIZE(buf->kvec));
	}
	spin_lock_bh(&net->data_lock);
	list_splice_init(bufs, &net->rx_free);
	spin_unlock_bh(&net->data_lock);
}

static void vduse_guest_net_put_rx_buf(struct vduse_guest_net *net,
				       struct vduse_guest_net_rx_buf *buf)
{
	LIST_HEAD(bufs);

	list_add(&buf->list, &bufs);
	vduse_guest_net_put_rx_bufs(net, &bufs);
}

/*
 * Take the buffers that the host posts, which can sleep, so that the rx
 * handler only copies into them. The handler returns a buffer to the pool
 * before the host can post its head again, so when enabling notifications
 * finds more heads posted, the pool has room for them. With EVENT_IDX the
 * host does not kick for heads that it posted before notifications were
 * enabled, so they are taken here.
 */
static void vduse_guest_net_rx_work(struct work_struct *work)
{
	struct vduse_guest_net *net =
		container_of(work, struct vduse_guest_net, rx_work);
	struct kvec rkvec[VDUSE_GUEST_NET_INLINE_IOVS];
	struct vduse_guest_net_rx_buf *buf;
	struct vringh_kiov riov;
	u64 features;
	bool running;
	int r;

	/* A head that is taken while the data path is stopped is never used. */
	if (!vduse_guest_net_data_running(net, &features))
		return;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));

	/*
	 * Under sustained traffic the work yields on each pass, so that it
	 * does not keep other tasks from the CPU. Other work on the
	 * workqueue waits until the workqueue counts this work as CPU
	 * intensive.
	 */
	for (;; cond_resched()) {
		buf = vduse_guest_net_get_rx_buf(net);
		if (!buf) {
			if (vduse_guest_notify_enable(net->dev,
						      VDUSE_GUEST_NET_RX))
				break;
			vduse_guest_notify_disable(net->dev, VDUSE_GUEST_NET_RX);
			continue;
		}

		r = vduse_guest_getdesc(net->dev, VDUSE_GUEST_NET_RX, &riov,
					&buf->wiov, &buf->head);
		if (r <= 0) {
			vduse_guest_net_put_rx_buf(net, buf);
			if (r < 0)
				break;
			if (vduse_guest_notify_enable(net->dev,
						      VDUSE_GUEST_NET_RX))
				break;
			vduse_guest_notify_disable(net->dev, VDUSE_GUEST_NET_RX);
			continue;
		}

		spin_lock_bh(&net->data_lock);
		running = net->data_running;
		if (running)
			list_add_tail(&buf->list, &net->rx_bufs);
		spin_unlock_bh(&net->data_lock);
		if (!running) {
			vduse_guest_net_put_rx_buf(net, buf);
			break;
		}
	}

	vringh_kiov_cleanup(&riov);
}

static bool vduse_guest_net_rx_gso_ok(const struct sk_buff *skb, u64 features)
{
	unsigned int type = skb_shinfo(skb)->gso_type;

	if (type & ~VDUSE_GUEST_NET_RX_GSO)
		return false;
	if ((type & SKB_GSO_TCP_ECN) &&
	    !(features & BIT_ULL(VIRTIO_NET_F_GUEST_ECN)))
		return false;
	if (type & SKB_GSO_TCPV4)
		return features & BIT_ULL(VIRTIO_NET_F_GUEST_TSO4);
	if (type & SKB_GSO_TCPV6)
		return features & BIT_ULL(VIRTIO_NET_F_GUEST_TSO6);
	return false;
}

/*
 * Copy a frame, from its MAC header, into as many of the host's buffers as
 * it needs, and complete them together. The skb is consumed.
 */
static void vduse_guest_net_rx_one(struct vduse_guest_net *net,
				   struct sk_buff *skb, u64 features)
{
	struct vring_used_elem used[VDUSE_GUEST_NET_MAX_BUFS];
	struct vduse_guest_net_rx_buf *buf;
	struct virtio_net_hdr_v1 hdr;
	unsigned int n = 0, max_bufs;
	size_t need, room = 0, off = 0, chunk, written;
	struct iov_iter it;
	LIST_HEAD(bufs);
	int err;

	if (skb->ip_summed == CHECKSUM_PARTIAL &&
	    !(features & BIT_ULL(VIRTIO_NET_F_GUEST_CSUM))) {
		err = skb_checksum_help(skb);
		if (err) {
			atomic64_inc(err == -ENOMEM ? &net->stats.rx_dropped :
				     &net->stats.rx_unsupported);
			goto drop;
		}
	}
	if (virtio_net_hdr_from_skb(skb, (struct virtio_net_hdr *)&hdr, true,
				    features & BIT_ULL(VIRTIO_NET_F_GUEST_CSUM),
				    0)) {
		atomic64_inc(&net->stats.rx_unsupported);
		goto drop;
	}

	need = sizeof(hdr) + skb->len;
	max_bufs = features & BIT_ULL(VIRTIO_NET_F_MRG_RXBUF) ?
		   VDUSE_GUEST_NET_MAX_BUFS : 1;

	spin_lock(&net->data_lock);
	while (room < need && n < max_bufs && net->data_running) {
		buf = list_first_entry_or_null(&net->rx_bufs,
					       struct vduse_guest_net_rx_buf,
					       list);
		if (!buf)
			break;
		list_move_tail(&buf->list, &bufs);
		room += vringh_kiov_length(&buf->wiov);
		n++;
	}
	if (room < need) {
		list_splice(&bufs, &net->rx_bufs);
		if (!net->data_running) {
			spin_unlock(&net->data_lock);
			goto drop;
		}
		spin_unlock(&net->data_lock);
		if (n == max_bufs)
			atomic64_inc(&net->stats.rx_oversized);
		else
			atomic64_inc(&net->stats.rx_no_buffer);
		goto drop;
	}
	spin_unlock(&net->data_lock);

	hdr.num_buffers = __cpu_to_virtio16(true, n);
	n = 0;
	list_for_each_entry(buf, &bufs, list) {
		iov_iter_kvec(&it, ITER_DEST, buf->wiov.iov, buf->wiov.used,
			      vringh_kiov_length(&buf->wiov));
		written = 0;
		if (!n) {
			written = copy_to_iter(&hdr, sizeof(hdr), &it);
			if (written != sizeof(hdr))
				off = skb->len;
		}
		chunk = min(iov_iter_count(&it), skb->len - off);
		if (skb_copy_datagram_iter(skb, off, &it, chunk))
			off = skb->len;
		else
			off += chunk;
		written += chunk;
		vduse_guest_used_elem(net->dev, VDUSE_GUEST_NET_RX, &used[n++],
				      buf->head, written);
	}
	/*
	 * The buffers go back to the pool before the host can post their
	 * heads again, so the rx work finds room for every head it is told
	 * of.
	 */
	vduse_guest_net_put_rx_bufs(net, &bufs);
	vduse_guest_complete_n(net->dev, VDUSE_GUEST_NET_RX, used, n);
	atomic64_inc(&net->stats.rx_frames);
	atomic64_add(n, &net->stats.rx_buffers);
	if (skb_is_gso(skb))
		atomic64_inc(&net->stats.rx_gso);
	consume_skb(skb);
	return;

drop:
	kfree_skb(skb);
}

/*
 * Every frame received on the interface goes to the host, and the guest's
 * own stack sees none. An aggregate of a GSO type that the host cannot take
 * goes as its segments. A stopping device waits for this handler to return.
 */
static rx_handler_result_t vduse_guest_net_rx(struct sk_buff **pskb)
{
	struct vduse_guest_net *net =
		rcu_dereference((*pskb)->dev->rx_handler_data);
	struct sk_buff *skb, *segs, *next;
	u64 features;

	skb = skb_share_check(*pskb, GFP_ATOMIC);
	if (!skb) {
		atomic64_inc(&net->stats.rx_dropped);
		return RX_HANDLER_CONSUMED;
	}

	if (!vduse_guest_net_data_running(net, &features)) {
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}

	/*
	 * The stack moves a VLAN tag out of the frame before the handler
	 * runs, and the host has no other way to receive it.
	 */
	skb_push(skb, skb->data - skb_mac_header(skb));
	if (skb_vlan_tag_present(skb)) {
		skb = __vlan_hwaccel_push_inside(skb);
		if (!skb) {
			atomic64_inc(&net->stats.rx_dropped);
			return RX_HANDLER_CONSUMED;
		}
	}

	if (!skb_is_gso(skb) || vduse_guest_net_rx_gso_ok(skb, features)) {
		vduse_guest_net_rx_one(net, skb, features);
		return RX_HANDLER_CONSUMED;
	}

	segs = __skb_gso_segment(skb, 0, false);
	if (IS_ERR_OR_NULL(segs)) {
		atomic64_inc(PTR_ERR_OR_ZERO(segs) == -ENOMEM ?
			     &net->stats.rx_dropped :
			     &net->stats.rx_unsupported);
		kfree_skb(skb);
		return RX_HANDLER_CONSUMED;
	}
	consume_skb(skb);
	atomic64_inc(&net->stats.rx_segmented);
	skb_list_walk_safe(segs, skb, next) {
		skb_mark_not_on_list(skb);
		vduse_guest_net_rx_one(net, skb, features);
	}
	return RX_HANDLER_CONSUMED;
}

/*
 * Stop the data path. Frames that the interface still holds keep their
 * count, and the buffers that the host posted are dropped. A reset returns
 * them to the host, and once the interface is gone the device passes no
 * more traffic.
 */
static void vduse_guest_net_stop_data(struct vduse_guest_net *net)
{
	LIST_HEAD(bufs);

	spin_lock_bh(&net->data_lock);
	net->data_running = false;
	list_splice_init(&net->rx_bufs, &bufs);
	spin_unlock_bh(&net->data_lock);

	cancel_work_sync(&net->rx_work);
	cancel_work_sync(&net->tx_work);
	vduse_guest_net_put_rx_bufs(net, &bufs);
}

/*
 * Bring the configuration space up to date with the link. While the device
 * runs, the host is told of each change with the configuration interrupt,
 * which VDUSE refuses until DRIVER_OK has been answered, so a refused one is
 * retried.
 */
static void vduse_guest_net_link_work(struct work_struct *work)
{
	struct vduse_guest_net *net =
		container_of(work, struct vduse_guest_net, link_work.work);
	struct virtio_net_config *c = &net->config;
	struct ethtool_link_ksettings ks;
	u32 speed = SPEED_UNKNOWN;
	u8 duplex = DUPLEX_UNKNOWN;
	u16 status = 0;
	int err;

	rtnl_lock();
	if (!net->gone && netif_running(net->netdev) &&
	    netif_carrier_ok(net->netdev)) {
		status = VIRTIO_NET_S_LINK_UP;
		if (!__ethtool_get_link_ksettings(net->netdev, &ks)) {
			speed = ks.base.speed;
			duplex = ks.base.duplex;
		}
	}

	if (c->status != __cpu_to_virtio16(true, status) ||
	    c->speed != cpu_to_le32(speed) || c->duplex != duplex) {
		c->status = __cpu_to_virtio16(true, status);
		c->speed = cpu_to_le32(speed);
		c->duplex = duplex;
		net->config_dirty = true;
		atomic64_inc(&net->stats.link_changes);
		pr_info("%s: link %s\n", vduse_guest_name(net->dev),
			status ? "up" : "down");
	}

	if (net->running && net->config_dirty) {
		err = vduse_guest_set_config(net->dev, 0, c,
					     VDUSE_GUEST_NET_CONFIG_SIZE);
		if (!err)
			err = vduse_guest_config_irq(net->dev);
		/* A host without configuration updates is not told. */
		if (!err || err == -EOPNOTSUPP)
			net->config_dirty = false;
		else if (err == -EINVAL)
			queue_delayed_work(system_dfl_wq, &net->link_work,
					   VDUSE_GUEST_NET_LINK_RETRY);
		else
			pr_warn_once("%s: cannot report the link: %d\n",
				     vduse_guest_name(net->dev), err);
	}
	rtnl_unlock();
}

/* The interface is being unregistered, with the RTNL. */
static void vduse_guest_net_gone(struct vduse_guest_net *net)
{
	net->gone = true;
	if (net->rx_registered) {
		netdev_rx_handler_unregister(net->netdev);
		net->rx_registered = false;
	}
	vduse_guest_net_stop_data(net);
	pr_warn("%s: interface %s is gone\n", vduse_guest_name(net->dev),
		net->netdev->name);
	netdev_put(net->netdev, &net->tracker);
	mod_delayed_work(system_dfl_wq, &net->link_work, 0);
}

static int vduse_guest_net_netdev_event(struct notifier_block *nb,
					unsigned long event, void *ptr)
{
	struct vduse_guest_net *net =
		container_of(nb, struct vduse_guest_net, nb);
	struct net_device *dev = netdev_notifier_info_to_dev(ptr);

	if (net->gone || dev != net->netdev)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
		mod_delayed_work(system_dfl_wq, &net->link_work, 0);
		break;
	case NETDEV_UNREGISTER:
		vduse_guest_net_gone(net);
		break;
	}
	return NOTIFY_DONE;
}

/* The data queues' work runs on the CPU that handles their kicks. */
static void vduse_guest_net_kick(void *priv, unsigned int index)
{
	struct vduse_guest_net *net = priv;

	if (index == VDUSE_GUEST_NET_RX) {
		queue_work(system_percpu_wq, &net->rx_work);
	} else {
		WRITE_ONCE(net->tx_cpu, raw_smp_processor_id());
		vduse_guest_net_queue_tx(net);
	}
}

/*
 * GRO, in software or in the NIC, merges received frames into segments that
 * only a host with every receive offload accepts, so it is on only then. The
 * interface's removal takes the RTNL, so it stops the data path either
 * before start or after it.
 */
static int vduse_guest_net_start(void *priv)
{
	struct vduse_guest_net *net = priv;
	struct net_device *netdev = net->netdev;
	u64 features = vduse_guest_driver_features(net->dev);
	int err;

	rtnl_lock();
	if (net->gone) {
		rtnl_unlock();
		return -ENODEV;
	}
	err = netdev_rx_handler_register(netdev, vduse_guest_net_rx, net);
	if (err) {
		rtnl_unlock();
		return err;
	}
	net->rx_registered = true;
	netdev_lock_ops(netdev);
	if ((features & VDUSE_GUEST_NET_GRO_FEATURES) ==
	    VDUSE_GUEST_NET_GRO_FEATURES)
		netdev->wanted_features |= NETIF_F_GRO;
	else
		netdev->wanted_features &= ~(NETIF_F_GRO | NETIF_F_GRO_HW);
	netdev->wanted_features &= ~NETIF_F_LRO;
	netdev_update_features(netdev);
	netdev_unlock_ops(netdev);
	net->running = true;
	net->config_dirty = true;
	spin_lock_bh(&net->data_lock);
	net->features = features;
	net->data_running = true;
	spin_unlock_bh(&net->data_lock);
	rtnl_unlock();

	/* The transport kicks each ready queue once start returns. */
	mod_delayed_work(system_dfl_wq, &net->link_work, 0);
	return 0;
}

/* The interface stays open, so that its link stays up across resets. */
static void vduse_guest_net_stop(void *priv)
{
	struct vduse_guest_net *net = priv;

	vduse_guest_net_stop_data(net);

	rtnl_lock();
	net->running = false;
	if (net->rx_registered) {
		netdev_rx_handler_unregister(net->netdev);
		net->rx_registered = false;
	}
	rtnl_unlock();
}

/*
 * Hold the interface, the named one or else the first Ethernet interface of
 * a device, which leaves out virtual interfaces. It is recorded under the
 * RTNL, so the notifier sees its unregistration from then on.
 */
static bool vduse_guest_net_find_netdev(struct vduse_guest_net *net,
					const char *name)
{
	struct net_device *dev;
	bool found = false;

	rtnl_lock();
	for_each_netdev(&init_net, dev) {
		if (name) {
			if (strcmp(dev->name, name))
				continue;
		} else if (dev->type != ARPHRD_ETHER || !dev->dev.parent) {
			continue;
		}
		netdev_hold(dev, &net->tracker, GFP_KERNEL);
		net->netdev = dev;
		found = true;
		break;
	}
	rtnl_unlock();
	return found;
}

struct vduse_guest_net_wait {
	struct notifier_block nb;
	struct completion registered;
};

static int vduse_guest_net_wait_event(struct notifier_block *nb,
				      unsigned long event, void *ptr)
{
	struct vduse_guest_net_wait *w =
		container_of(nb, struct vduse_guest_net_wait, nb);

	if (event == NETDEV_REGISTER)
		complete(&w->registered);
	return NOTIFY_DONE;
}

/*
 * Find the interface. A driver can register its interface from a work item,
 * so this waits for one to appear, up to the timeout.
 */
static int vduse_guest_net_attach(struct vduse_guest_net *net,
				  const char *name, unsigned int timeout)
{
	unsigned long deadline = jiffies + timeout * HZ;
	struct vduse_guest_net_wait w = {
		.nb.notifier_call = vduse_guest_net_wait_event,
	};
	int err;

	init_completion(&w.registered);
	wait_for_device_probe();
	err = register_netdevice_notifier(&w.nb);
	if (err)
		return err;

	for (;;) {
		if (vduse_guest_net_find_netdev(net, name))
			break;
		if (time_after(jiffies, deadline)) {
			pr_err("%s: no Ethernet interface%s%s appeared in %u seconds\n",
			       vduse_guest_name(net->dev), name ? " " : "",
			       name ?: "", timeout);
			err = -ENODEV;
			break;
		}
		wait_for_completion_timeout(&w.registered, HZ / 10);
		reinit_completion(&w.registered);
	}

	unregister_netdevice_notifier(&w.nb);
	return err;
}

/*
 * Open the interface for the host. All-multicast mode passes the multicast
 * groups that the host joins, since the host has no way to name them.
 */
static int vduse_guest_net_open(struct vduse_guest_net *net, int mtu)
{
	struct net_device *netdev = net->netdev;
	int err;

	rtnl_lock();
	if (net->gone) {
		err = -ENODEV;
		goto unlock;
	}
	net->was_up = netdev->flags & IFF_UP;
	net->saved_mtu = netdev->mtu;
	if (mtu) {
		err = dev_set_mtu(netdev, mtu);
		if (err) {
			pr_err("%s: cannot set the MTU of %s to %d: %d\n",
			       vduse_guest_name(net->dev), netdev->name, mtu,
			       err);
			goto unlock;
		}
	}
	err = dev_set_allmulti(netdev, 1);
	if (err)
		goto restore_mtu;
	err = dev_open(netdev, NULL);
	if (err) {
		dev_set_allmulti(netdev, -1);
		goto restore_mtu;
	}
	net->opened = true;

	ether_addr_copy(net->config.mac, netdev->dev_addr);
	net->config.mtu = __cpu_to_virtio16(true, netdev->mtu);
	net->config.speed = cpu_to_le32(SPEED_UNKNOWN);
	net->config.duplex = DUPLEX_UNKNOWN;
	net->create_config = net->config;
	strscpy(net->ifname, netdev->name);
	rtnl_unlock();
	return 0;

restore_mtu:
	if (netdev->mtu != net->saved_mtu)
		dev_set_mtu(netdev, net->saved_mtu);
unlock:
	rtnl_unlock();
	return err;
}

/*
 * Let the interface go when the device cannot be created: undo what the
 * open changed and drop the reference. Marking the interface gone first
 * keeps the notifier from treating the unregistration that
 * unregister_netdevice_notifier() replays as the interface's removal.
 */
static void vduse_guest_net_detach(struct vduse_guest_net *net)
{
	struct net_device *netdev = net->netdev;

	rtnl_lock();
	if (!net->gone) {
		net->gone = true;
		if (net->opened) {
			if (!net->was_up)
				dev_close(netdev);
			dev_set_allmulti(netdev, -1);
			if (netdev->mtu != net->saved_mtu)
				dev_set_mtu(netdev, net->saved_mtu);
		}
		netdev_put(netdev, &net->tracker);
	}
	rtnl_unlock();
}

static void *__init vduse_guest_net_setup(struct vduse_guest_dev *dev,
					  char *opts,
					  struct vduse_guest_config *cfg)
{
	unsigned int timeout = VDUSE_GUEST_NET_TIMEOUT;
	struct vduse_guest_net *net;
	char *netdev = NULL, *opt;
	unsigned int i;
	int mtu = 0;
	int err;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "netdev=")) {
			netdev = opt + strlen("netdev=");
		} else if (strstarts(opt, "mtu=")) {
			if (kstrtoint(opt + strlen("mtu="), 10, &mtu) ||
			    mtu < ETH_MIN_MTU || mtu > U16_MAX)
				return ERR_PTR(-EINVAL);
		} else if (strstarts(opt, "timeout=")) {
			if (kstrtouint(opt + strlen("timeout="), 10, &timeout))
				return ERR_PTR(-EINVAL);
		} else if (*opt) {
			return ERR_PTR(-EINVAL);
		}
	}

	net = kzalloc_obj(*net);
	if (!net)
		return ERR_PTR(-ENOMEM);
	net->dev = dev;
	INIT_WORK(&net->rx_work, vduse_guest_net_rx_work);
	INIT_WORK(&net->tx_work, vduse_guest_net_tx_work);
	net->tx_cpu = WORK_CPU_UNBOUND;
	INIT_DELAYED_WORK(&net->link_work, vduse_guest_net_link_work);
	spin_lock_init(&net->data_lock);
	INIT_LIST_HEAD(&net->rx_bufs);
	INIT_LIST_HEAD(&net->rx_free);

	net->rx_pool = kvcalloc(VDUSE_GUEST_NET_RX_SIZE, sizeof(*net->rx_pool),
				GFP_KERNEL);
	if (!net->rx_pool) {
		err = -ENOMEM;
		goto free;
	}
	for (i = 0; i < VDUSE_GUEST_NET_RX_SIZE; i++) {
		struct vduse_guest_net_rx_buf *buf = &net->rx_pool[i];

		vringh_kiov_init(&buf->wiov, buf->kvec, ARRAY_SIZE(buf->kvec));
		list_add_tail(&buf->list, &net->rx_free);
	}

	/* The notifier ignores every interface until one is held. */
	net->nb.notifier_call = vduse_guest_net_netdev_event;
	err = register_netdevice_notifier(&net->nb);
	if (err)
		goto free;
	err = vduse_guest_net_attach(net, netdev, timeout);
	if (err)
		goto unregister;
	err = vduse_guest_net_open(net, mtu);
	if (err)
		goto detach;

	cfg->device_id = VIRTIO_ID_NET;
	cfg->features = VDUSE_GUEST_NET_FEATURES;
	cfg->num_queues = VDUSE_GUEST_NET_NUM_QUEUES;
	cfg->queue_sizes = vduse_guest_net_queue_sizes;
	cfg->allow_reconnect = true;
	cfg->config = &net->create_config;
	cfg->config_size = VDUSE_GUEST_NET_CONFIG_SIZE;

	pr_info("%s: serving %s, %pM, MTU %u\n", vduse_guest_name(dev),
		net->ifname, net->create_config.mac,
		__virtio16_to_cpu(true, net->create_config.mtu));
	return net;

detach:
	vduse_guest_net_detach(net);
unregister:
	unregister_netdevice_notifier(&net->nb);
	cancel_delayed_work_sync(&net->link_work);
free:
	kvfree(net->rx_pool);
	kfree(net);
	return ERR_PTR(err);
}

static void vduse_guest_net_release(void *priv)
{
	struct vduse_guest_net *net = priv;

	vduse_guest_net_detach(net);
	unregister_netdevice_notifier(&net->nb);
	cancel_delayed_work_sync(&net->link_work);
	kvfree(net->rx_pool);
	kfree(net);
}

static void vduse_guest_net_show(void *priv, struct seq_file *m)
{
	struct vduse_guest_net *net = priv;
	struct vduse_guest_net_stats *s = &net->stats;

	seq_printf(m, "  %-18s %lld\n", "tx_frames",
		   atomic64_read(&s->tx_frames));
	seq_printf(m, "  %-18s %lld\n", "tx_gso", atomic64_read(&s->tx_gso));
	seq_printf(m, "  %-18s %lld\n", "tx_invalid",
		   atomic64_read(&s->tx_invalid));
	seq_printf(m, "  %-18s %lld\n", "tx_dropped",
		   atomic64_read(&s->tx_dropped));
	seq_printf(m, "  %-18s %lld\n", "tx_pending",
		   (long long)atomic_read(&net->tx_pending));
	seq_printf(m, "  %-18s %lld\n", "rx_frames",
		   atomic64_read(&s->rx_frames));
	seq_printf(m, "  %-18s %lld\n", "rx_gso", atomic64_read(&s->rx_gso));
	seq_printf(m, "  %-18s %lld\n", "rx_buffers",
		   atomic64_read(&s->rx_buffers));
	seq_printf(m, "  %-18s %lld\n", "rx_no_buffer",
		   atomic64_read(&s->rx_no_buffer));
	seq_printf(m, "  %-18s %lld\n", "rx_oversized",
		   atomic64_read(&s->rx_oversized));
	seq_printf(m, "  %-18s %lld\n", "rx_unsupported",
		   atomic64_read(&s->rx_unsupported));
	seq_printf(m, "  %-18s %lld\n", "rx_segmented",
		   atomic64_read(&s->rx_segmented));
	seq_printf(m, "  %-18s %lld\n", "rx_dropped",
		   atomic64_read(&s->rx_dropped));
	seq_printf(m, "  %-18s %lld\n", "link_changes",
		   atomic64_read(&s->link_changes));
}

const struct vduse_guest_backend_ops vduse_guest_net_ops = {
	.type = "net",
	.setup = vduse_guest_net_setup,
	.release = vduse_guest_net_release,
	.start = vduse_guest_net_start,
	.stop = vduse_guest_net_stop,
	.kick = vduse_guest_net_kick,
	.show = vduse_guest_net_show,
};
