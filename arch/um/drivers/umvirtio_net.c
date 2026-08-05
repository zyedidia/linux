// SPDX-License-Identifier: GPL-2.0
/*
 * umvirtio-net: expose a UML-visible network device to the host bridge.
 *
 * The point of this shim is to sit in front of a driver UML owns for real
 * hardware -- an igb port passed through with CONFIG_UML_PCI_OVER_VFIO --
 * and re-present it to the host as a virtio-net device. It behaves like a
 * bridge port: the netdev is put in promiscuous mode and every frame it
 * delivers is stolen by an rx_handler and relayed to the host; frames the
 * host sends are copied into fresh skbs and handed to dev_queue_xmit().
 *
 * Queue 0 is the receiveq and queue 1 the transmitq, as virtio-net
 * numbers them. RX requests are host-posted buffers that sit in a pending
 * list until a frame arrives; TX requests are handled as they come.
 *
 * No offload features are offered, so every frame crossing the ring is a
 * plain sub-MTU ethernet frame with a zeroed virtio_net_hdr. GRO is
 * disabled on the underlying netdev to keep aggregated super-frames from
 * exceeding the host's small-mode RX buffers.
 *
 * Mainline VDUSE rejects VIRTIO_NET_F_CTRL_VQ, so by default it is not
 * offered and promiscuous mode on the real port papers over the missing
 * RX filtering. Against a host kernel patched to allow it, ctrl=1 adds
 * the control queue (queue 2) and maps the host's RX-mode, MAC-table
 * and MAC-address commands onto the real netdev, at which point
 * promisc=0 becomes honest: the port filters exactly what the host
 * asked for.
 */

#define pr_fmt(fmt) "umvirtio-net: " fmt

#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/if_vlan.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/moduleparam.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/virtio_byteorder.h>
#include <net/netdev_lock.h>

#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_net.h>

#include <init.h>

#include "umvirtio.h"

/*
 * Geometry. Without guest GSO features the host posts MTU-sized RX
 * buffers and segments its TX to the MTU, so a slot only has to hold one
 * 1518-byte frame plus the 12-byte header. Total UML memory spent on
 * slots is queues * 2 * queue_size * slot_size = 2 MiB (3 MiB with the
 * control queue) at these numbers.
 */
#define UMV_NET_QUEUE_SIZE	256
#define UMV_NET_SLOT_SIZE	2048

#define UMV_NET_RXQ		0
#define UMV_NET_TXQ		1
#define UMV_NET_CTRLQ		2

/* Bound on each virtio_net_ctrl_mac table; 2 * 128 MACs fits a slot. */
#define UMV_NET_MAC_TABLE_MAX	128

#define UMV_NET_HDR_LEN		((u32)sizeof(struct virtio_net_hdr_v1))

struct umv_net {
	struct umv_dev *dev;
	struct net_device *netdev;

	/* Host RX buffers waiting for a frame, in arrival order. */
	spinlock_t rx_lock;
	struct list_head rx_pending;

	struct notifier_block nb;

	bool opened;
	bool promisc;
	bool handler;
	bool notifier;

	/* RX-mode state the host has commanded over the control queue. */
	bool ctrl_promisc;
	bool ctrl_allmulti;

	/* Diagnostics, dumped by the umvirtio_net.stats_secs= timer. */
	u32 n_pending;
	u64 rx_frames;
	u64 rx_drop_nobuf;
	u64 rx_drop_room;
	u64 tx_frames;
	u64 tx_drops;
	struct timer_list stats_timer;
};

struct umv_net_rxbuf {
	struct list_head node;
	struct umv_request *req;
};

static char umv_net_ifname[IFNAMSIZ] = "eth0";
static bool umv_net_promisc = true;
static bool umv_net_ctrl_enable;
static unsigned int umv_net_stats_secs;

static struct umv_net umv_net_dev;

/* ------------------------------------------------------------------ */
/* iov helpers. Entries are plain kernel pointers on the _kern path.    */
/* ------------------------------------------------------------------ */

static int umv_net_pull(struct vringh_kiov *kiov, void *dst, size_t len)
{
	u8 *out = dst;

	while (len) {
		struct kvec *v;
		size_t n;

		if (kiov->i >= kiov->used)
			return -EINVAL;

		v = &kiov->iov[kiov->i];
		n = min(len, v->iov_len);
		memcpy(out, v->iov_base, n);

		out += n;
		len -= n;
		v->iov_base = (u8 *)v->iov_base + n;
		v->iov_len -= n;
		if (!v->iov_len)
			kiov->i++;
	}

	return 0;
}

static int umv_net_push(struct vringh_kiov *kiov, const void *src, size_t len)
{
	const u8 *in = src;

	while (len) {
		struct kvec *v;
		size_t n;

		if (kiov->i >= kiov->used)
			return -EINVAL;

		v = &kiov->iov[kiov->i];
		n = min(len, v->iov_len);
		memcpy(v->iov_base, in, n);

		in += n;
		len -= n;
		v->iov_base = (u8 *)v->iov_base + n;
		v->iov_len -= n;
		if (!v->iov_len)
			kiov->i++;
	}

	return 0;
}

/* Copy the whole skb into kiov, advancing it. Frames may be nonlinear. */
static int umv_net_push_skb(struct vringh_kiov *kiov, struct sk_buff *skb)
{
	unsigned int off = 0;

	while (off < skb->len) {
		struct kvec *v;
		size_t n;

		if (kiov->i >= kiov->used)
			return -EINVAL;

		v = &kiov->iov[kiov->i];
		n = min_t(size_t, skb->len - off, v->iov_len);
		if (skb_copy_bits(skb, off, v->iov_base, n) < 0)
			return -EINVAL;

		off += n;
		v->iov_base = (u8 *)v->iov_base + n;
		v->iov_len -= n;
		if (!v->iov_len)
			kiov->i++;
	}

	return 0;
}

static size_t umv_net_iov_len(const struct vringh_kiov *kiov)
{
	size_t total = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++)
		total += kiov->iov[i].iov_len;

	return total;
}

/* ------------------------------------------------------------------ */
/* RX: netdev -> host                                                  */
/* ------------------------------------------------------------------ */

static rx_handler_result_t umv_net_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct umv_net *net = rcu_dereference(skb->dev->rx_handler_data);
	struct virtio_net_hdr_v1 hdr;
	struct umv_net_rxbuf *buf;
	struct umv_request *req;

	if (skb->pkt_type == PACKET_LOOPBACK)
		return RX_HANDLER_PASS;

	/* The frame gets rewritten below (push, vlan untag), so unshare. */
	skb = skb_share_check(skb, GFP_ATOMIC);
	if (!skb)
		return RX_HANDLER_CONSUMED;
	*pskb = skb;

	/* eth_type_trans() pulled the link header; the host wants it back. */
	if (skb_mac_header_was_set(skb))
		skb_push(skb, skb->data - skb_mac_header(skb));

	/* If hardware stripped a VLAN tag, put it back on the wire. */
	if (skb_vlan_tag_present(skb)) {
		skb = __vlan_hwaccel_push_inside(skb);
		if (!skb)
			return RX_HANDLER_CONSUMED;
		*pskb = skb;
	}

	spin_lock(&net->rx_lock);
	buf = list_first_entry_or_null(&net->rx_pending, struct umv_net_rxbuf,
				       node);
	if (buf) {
		list_del(&buf->node);
		net->n_pending--;
	}
	spin_unlock(&net->rx_lock);

	/* No host buffer posted: drop, as a real NIC with a full ring would. */
	if (!buf) {
		net->rx_drop_nobuf++;
		goto drop;
	}

	req = buf->req;
	kfree(buf);

	memset(&hdr, 0, sizeof(hdr));
	hdr.gso_type = VIRTIO_NET_HDR_GSO_NONE;
	hdr.num_buffers = __cpu_to_virtio16(true, 1);

	if (UMV_NET_HDR_LEN + skb->len > umv_net_iov_len(&req->wiov) ||
	    umv_net_push(&req->wiov, &hdr, sizeof(hdr)) < 0 ||
	    umv_net_push_skb(&req->wiov, skb) < 0) {
		/* Buffer too small for the frame; give it back empty. */
		net->rx_drop_room++;
		umv_request_complete(req, 0);
		goto drop;
	}

	net->rx_frames++;
	umv_request_complete(req, UMV_NET_HDR_LEN + skb->len);

	consume_skb(skb);
	return RX_HANDLER_CONSUMED;

drop:
	kfree_skb(skb);
	return RX_HANDLER_CONSUMED;
}

/* ------------------------------------------------------------------ */
/* TX: host -> netdev                                                  */
/* ------------------------------------------------------------------ */

static void umv_net_xmit(struct umv_net *net, struct umv_request *req)
{
	struct virtio_net_hdr_v1 hdr;
	struct sk_buff *skb;
	size_t len;

	if (umv_net_pull(&req->riov, &hdr, sizeof(hdr)) < 0)
		goto drop;

	/*
	 * No csum or GSO features were offered, so flags and gso_type must
	 * be zero and the frame is complete as-is. Anything else is a host
	 * driver bug; drop rather than transmit garbage.
	 */
	if (hdr.flags || hdr.gso_type != VIRTIO_NET_HDR_GSO_NONE)
		goto drop;

	len = umv_net_iov_len(&req->riov);
	if (len < ETH_HLEN || len > UMV_NET_SLOT_SIZE)
		goto drop;

	skb = netdev_alloc_skb(net->netdev, len);
	if (!skb)
		goto drop;

	if (umv_net_pull(&req->riov, skb_put(skb, len), len) < 0) {
		kfree_skb(skb);
		goto drop;
	}

	skb_reset_mac_header(skb);
	skb->protocol = dev_parse_header_protocol(skb);

	/* Consumes the skb whatever happens; drops count against the qdisc. */
	dev_queue_xmit(skb);

	net->tx_frames++;
	umv_request_complete(req, 0);
	return;

drop:
	net->tx_drops++;
	umv_request_complete(req, 0);
}

static void umv_net_ctrl_req(struct umv_net *net, struct umv_request *req);

static void umv_net_handle(struct umv_dev *dev, struct umv_request *req)
{
	struct umv_net *net = dev->priv;
	u32 queue = umv_request_queue(req);

	if (queue == UMV_NET_TXQ) {
		umv_net_xmit(net, req);
		return;
	}

	if (queue == UMV_NET_CTRLQ && umv_net_ctrl_enable) {
		umv_net_ctrl_req(net, req);
		return;
	}

	if (queue == UMV_NET_RXQ) {
		struct umv_net_rxbuf *buf;

		buf = kmalloc(sizeof(*buf), GFP_KERNEL);
		if (!buf) {
			umv_request_complete(req, 0);
			return;
		}

		buf->req = req;

		spin_lock_bh(&net->rx_lock);
		list_add_tail(&buf->node, &net->rx_pending);
		net->n_pending++;
		spin_unlock_bh(&net->rx_lock);
		return;
	}

	umv_request_complete(req, 0);
}

/* ------------------------------------------------------------------ */
/* Control queue: host commands -> netdev state                        */
/* ------------------------------------------------------------------ */

static u8 umv_net_ctrl_rx(struct umv_net *net, u8 cmd,
			  struct vringh_kiov *riov)
{
	int err;
	u8 on;

	if (umv_net_pull(riov, &on, sizeof(on)) < 0)
		return VIRTIO_NET_ERR;
	on = !!on;

	switch (cmd) {
	case VIRTIO_NET_CTRL_RX_PROMISC:
		if (on == net->ctrl_promisc)
			return VIRTIO_NET_OK;
		rtnl_lock();
		err = dev_set_promiscuity(net->netdev, on ? 1 : -1);
		rtnl_unlock();
		if (err < 0)
			return VIRTIO_NET_ERR;
		net->ctrl_promisc = on;
		return VIRTIO_NET_OK;

	case VIRTIO_NET_CTRL_RX_ALLMULTI:
		if (on == net->ctrl_allmulti)
			return VIRTIO_NET_OK;
		rtnl_lock();
		err = dev_set_allmulti(net->netdev, on ? 1 : -1);
		rtnl_unlock();
		if (err < 0)
			return VIRTIO_NET_ERR;
		net->ctrl_allmulti = on;
		return VIRTIO_NET_OK;
	}

	return VIRTIO_NET_ERR;
}

/* One virtio_net_ctrl_mac table: le32 count, then that many MACs. */
static int umv_net_pull_mac_table(struct vringh_kiov *riov,
				  struct net_device *ndev,
				  int (*add)(struct net_device *,
					     const unsigned char *),
				  u32 *n_out)
{
	u8 mac[ETH_ALEN];
	__virtio32 wire;
	u32 n, i;

	if (umv_net_pull(riov, &wire, sizeof(wire)) < 0)
		return -EINVAL;

	n = __virtio32_to_cpu(true, wire);
	if (n > UMV_NET_MAC_TABLE_MAX)
		return -EINVAL;

	for (i = 0; i < n; i++) {
		if (umv_net_pull(riov, mac, ETH_ALEN) < 0)
			return -EINVAL;
		add(ndev, mac);
	}

	*n_out = n;
	return 0;
}

static u8 umv_net_ctrl_mac(struct umv_net *net, u8 cmd,
			   struct vringh_kiov *riov)
{
	struct net_device *ndev = net->netdev;

	if (cmd == VIRTIO_NET_CTRL_MAC_ADDR_SET) {
		struct umv_dev *dev = net->dev;
		struct sockaddr_storage ss;
		int err;

		memset(&ss, 0, sizeof(ss));
		ss.ss_family = ndev->type;
		if (umv_net_pull(riov, ((struct sockaddr *)&ss)->sa_data,
				 ETH_ALEN) < 0)
			return VIRTIO_NET_ERR;

		rtnl_lock();
		err = dev_set_mac_address(ndev, &ss, NULL);
		rtnl_unlock();
		if (err < 0)
			return VIRTIO_NET_ERR;

		/* Keep the config-space copy telling the same story. */
		memcpy(dev->config + offsetof(struct virtio_net_config, mac),
		       ndev->dev_addr, ETH_ALEN);
		umv_notify_config(dev,
				  offsetof(struct virtio_net_config, mac),
				  ETH_ALEN);
		return VIRTIO_NET_OK;
	}

	if (cmd == VIRTIO_NET_CTRL_MAC_TABLE_SET) {
		u32 uc = 0, mc = 0;
		int err;

		/*
		 * The driver resends both full tables every time, so mirror
		 * by rebuilding. Nothing else in this kernel touches the
		 * lists, so flushing cannot step on another user.
		 */
		rtnl_lock();
		dev_uc_flush(ndev);
		dev_mc_flush(ndev);
		err = umv_net_pull_mac_table(riov, ndev, dev_uc_add, &uc);
		if (!err)
			err = umv_net_pull_mac_table(riov, ndev, dev_mc_add,
						     &mc);
		/*
		 * Each add syncs the hardware filter as a side effect; an
		 * all-empty update has no add to piggyback on, so force a
		 * sync with a throwaway add/del pair.
		 */
		if (!uc && !mc) {
			dev_uc_add(ndev, ndev->dev_addr);
			dev_uc_del(ndev, ndev->dev_addr);
		}
		rtnl_unlock();

		return err ? VIRTIO_NET_ERR : VIRTIO_NET_OK;
	}

	return VIRTIO_NET_ERR;
}

static void umv_net_ctrl_req(struct umv_net *net, struct umv_request *req)
{
	struct virtio_net_ctrl_hdr hdr;
	u8 status = VIRTIO_NET_ERR;
	struct kvec *ackv;

	/* The ack is the first (and only) device-writable byte. */
	if (req->wiov.i >= req->wiov.used ||
	    !req->wiov.iov[req->wiov.i].iov_len) {
		umv_request_complete(req, 0);
		return;
	}
	ackv = &req->wiov.iov[req->wiov.i];

	if (umv_net_pull(&req->riov, &hdr, sizeof(hdr)) == 0) {
		switch (hdr.class) {
		case VIRTIO_NET_CTRL_RX:
			status = umv_net_ctrl_rx(net, hdr.cmd, &req->riov);
			break;
		case VIRTIO_NET_CTRL_MAC:
			status = umv_net_ctrl_mac(net, hdr.cmd, &req->riov);
			break;
		}
	}

	*(u8 *)ackv->iov_base = status;
	umv_request_complete(req, 1);
}

/* ------------------------------------------------------------------ */
/* Link status                                                         */
/* ------------------------------------------------------------------ */

static void umv_net_update_status(struct umv_net *net, bool notify)
{
	struct umv_dev *dev = net->dev;
	u16 status = 0;
	__virtio16 wire;

	if (netif_running(net->netdev) && netif_carrier_ok(net->netdev))
		status = VIRTIO_NET_S_LINK_UP;

	wire = __cpu_to_virtio16(true, status);

	if (!memcmp(dev->config + offsetof(struct virtio_net_config, status),
		    &wire, sizeof(wire)))
		return;

	memcpy(dev->config + offsetof(struct virtio_net_config, status),
	       &wire, sizeof(wire));

	if (notify)
		umv_notify_config(dev,
				  offsetof(struct virtio_net_config, status),
				  sizeof(wire));
}

static int umv_net_netdev_event(struct notifier_block *nb, unsigned long event,
				void *ptr)
{
	struct umv_net *net = container_of(nb, struct umv_net, nb);
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);

	if (ndev != net->netdev)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_UP:
	case NETDEV_DOWN:
	case NETDEV_CHANGE:
		umv_net_update_status(net, READ_ONCE(net->dev->started));
		break;
	}

	return NOTIFY_DONE;
}

static void umv_net_stats_fn(struct timer_list *t)
{
	struct umv_net *net = timer_container_of(net, t, stats_timer);

	pr_info("rx %llu (drop nobuf %llu room %llu) tx %llu (drop %llu) pending %u\n",
		net->rx_frames, net->rx_drop_nobuf, net->rx_drop_room,
		net->tx_frames, net->tx_drops, net->n_pending);

	mod_timer(&net->stats_timer, jiffies + umv_net_stats_secs * HZ);
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static int umv_net_setup(struct umv_dev *dev)
{
	struct umv_net *net = dev->priv;
	struct virtio_net_config cfg;
	struct net_device *ndev;
	int err;

	/* The igb probe may still be running when this initcall fires. */
	wait_for_device_probe();

	ndev = dev_get_by_name(&init_net, umv_net_ifname);
	if (!ndev) {
		pr_err("no such interface: %s\n", umv_net_ifname);
		return -ENODEV;
	}

	net->netdev = ndev;
	net->dev = dev;
	spin_lock_init(&net->rx_lock);
	INIT_LIST_HEAD(&net->rx_pending);

	rtnl_lock();

	/*
	 * Everything on the wire must stay a discrete sub-MTU frame: no
	 * guest GSO features are offered, so the host's RX buffers cannot
	 * take aggregated super-frames.
	 */
	netdev_lock_ops(ndev);
	netif_disable_lro(ndev);
	ndev->wanted_features &= ~NETIF_F_GRO;
	netdev_update_features(ndev);
	netdev_unlock_ops(ndev);

	err = dev_open(ndev, NULL);
	if (err < 0) {
		pr_err("cannot open %s: %d\n", umv_net_ifname, err);
		goto err_unlock;
	}
	net->opened = true;

	if (umv_net_promisc) {
		err = dev_set_promiscuity(ndev, 1);
		if (err < 0) {
			pr_err("cannot set %s promiscuous: %d\n",
			       umv_net_ifname, err);
			goto err_unlock;
		}
		net->promisc = true;
	}

	err = netdev_rx_handler_register(ndev, umv_net_handle_frame, net);
	if (err < 0) {
		pr_err("cannot register rx handler on %s: %d\n",
		       umv_net_ifname, err);
		goto err_unlock;
	}
	net->handler = true;

	rtnl_unlock();

	net->nb.notifier_call = umv_net_netdev_event;
	err = register_netdevice_notifier(&net->nb);
	if (err < 0)
		return err;
	net->notifier = true;

	dev->device_id = VIRTIO_ID_NET;
	dev->vendor_id = 0;
	dev->num_queues = umv_net_ctrl_enable ? 3 : 2;
	dev->queue_size = UMV_NET_QUEUE_SIZE;
	dev->slot_size = UMV_NET_SLOT_SIZE;

	dev->features = (1ULL << VIRTIO_F_VERSION_1) |
			(1ULL << VIRTIO_F_ACCESS_PLATFORM) |
			(1ULL << VIRTIO_NET_F_MAC) |
			(1ULL << VIRTIO_NET_F_STATUS) |
			(1ULL << VIRTIO_NET_F_MTU);
	if (umv_net_ctrl_enable)
		dev->features |= (1ULL << VIRTIO_NET_F_CTRL_VQ) |
				 (1ULL << VIRTIO_NET_F_CTRL_RX) |
				 (1ULL << VIRTIO_NET_F_CTRL_MAC_ADDR);

	memset(&cfg, 0, sizeof(cfg));
	memcpy(cfg.mac, ndev->dev_addr, ETH_ALEN);
	cfg.mtu = __cpu_to_virtio16(true, (u16)ndev->mtu);
	cfg.max_virtqueue_pairs = __cpu_to_virtio16(true, 1);

	memcpy(dev->config, &cfg, sizeof(cfg));
	dev->config_size = sizeof(cfg);

	/* Sets the status field; carrier may already be up. */
	umv_net_update_status(net, false);

	if (umv_net_stats_secs) {
		timer_setup(&net->stats_timer, umv_net_stats_fn, 0);
		mod_timer(&net->stats_timer,
			  jiffies + umv_net_stats_secs * HZ);
	}

	pr_info("exporting %s, %pM, mtu %u\n", umv_net_ifname,
		ndev->dev_addr, ndev->mtu);

	return 0;

err_unlock:
	rtnl_unlock();
	return err;
}

static int umv_net_start(struct umv_dev *dev, u64 features)
{
	struct umv_net *net = dev->priv;

	/*
	 * The carrier may have come up between HELLO and now; the notifier
	 * did not push it because the socket was not ready. Re-sync, and
	 * notify so the host picks up a change to the snapshot it read.
	 */
	umv_net_update_status(net, true);

	return 0;
}

static void umv_net_remove(struct umv_dev *dev)
{
	struct umv_net *net = dev->priv;
	struct umv_net_rxbuf *buf, *tmp;

	if (net->notifier)
		unregister_netdevice_notifier(&net->nb);

	if (net->netdev) {
		rtnl_lock();
		if (net->handler)
			netdev_rx_handler_unregister(net->netdev);
		if (net->promisc)
			dev_set_promiscuity(net->netdev, -1);
		if (net->ctrl_promisc)
			dev_set_promiscuity(net->netdev, -1);
		if (net->ctrl_allmulti)
			dev_set_allmulti(net->netdev, -1);
		rtnl_unlock();
	}

	list_for_each_entry_safe(buf, tmp, &net->rx_pending, node) {
		list_del(&buf->node);
		umv_request_complete(buf->req, 0);
		kfree(buf);
	}

	if (net->netdev) {
		dev_put(net->netdev);
		net->netdev = NULL;
	}
}

static const struct umv_device_ops umv_net_ops = {
	.name = "umvirtio-net",
	.setup = umv_net_setup,
	.start = umv_net_start,
	.handle = umv_net_handle,
	.remove = umv_net_remove,
};

static int __init umv_net_init(void)
{
	return umv_register_device(&umv_net_ops, &umv_net_dev);
}
late_initcall(umv_net_init);

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

module_param_string(dev, umv_net_ifname, sizeof(umv_net_ifname), 0400);
__uml_help(umv_net_ifname,
"umvirtio_net.dev=<interface>\n"
"    Network interface to export to the host. There is no udev in this\n"
"    configuration, so interfaces have kernel names: the first probed\n"
"    port is eth0, which is the default.\n\n"
);

module_param_named(promisc, umv_net_promisc, bool, 0400);
__uml_help(umv_net_promisc,
"umvirtio_net.promisc=0\n"
"    Do not put the exported interface in promiscuous mode. Without\n"
"    ctrl=1 the host cannot program RX filters, so only unicast to the\n"
"    device's own MAC and broadcast will be received.\n\n"
);

module_param_named(ctrl, umv_net_ctrl_enable, bool, 0400);
__uml_help(umv_net_ctrl_enable,
"umvirtio_net.ctrl=1\n"
"    Offer a virtio-net control queue (CTRL_VQ, CTRL_RX, CTRL_MAC_ADDR)\n"
"    and apply the host's RX-mode, MAC-table and MAC-address commands\n"
"    to the exported interface. Mainline VDUSE rejects CTRL_VQ, so this\n"
"    needs a patched host kernel; combine with promisc=0 to let the\n"
"    host drive filtering.\n\n"
);

module_param_named(stats_secs, umv_net_stats_secs, uint, 0400);
__uml_help(umv_net_stats_secs,
"umvirtio_net.stats_secs=<n>\n"
"    Print frame and drop counters to the console every <n> seconds.\n"
"    0 (default) disables.\n\n"
);
