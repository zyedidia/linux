// SPDX-License-Identifier: GPL-2.0
/*
 * umvirtio: generic transport between a UML device shim and a host bridge.
 *
 * UML is the *device* side of each vring (via vringh) and the host is the
 * *driver* side, which is inverted from normal virtio. That inversion is
 * deliberate: it lets every shared buffer be ordinary UML kernel memory,
 * so vringh works on plain pointers and a shim can hand descriptor
 * payloads straight to a real driver without bouncing them.
 *
 * The host maps UML's physmem fd in its entirety and translates addresses
 * with a fixed offset, so descriptors carry kernel virtual addresses. UML
 * fills desc->addr in once at startup and the host only ever writes len,
 * flags and next.
 *
 * See umvirtio_proto.h for the wire format.
 */

#define pr_fmt(fmt) "umvirtio: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/virtio_ring.h>
#include <linux/vringh.h>
#include <linux/workqueue.h>

#include <as-layout.h>
#include <init.h>
#include <irq_kern.h>
#include <mem.h>
#include <os.h>

#include "umvirtio.h"

#define UMV_DEF_QUEUE_SIZE	64
#define UMV_DEF_SLOT_SIZE	(128 * 1024)

/*
 * Chains are at most two descriptors by construction (see the layout note
 * in umvirtio_proto.h), but leave headroom so a malformed chain from the
 * host is rejected by vringh rather than overrunning the iov array.
 */
#define UMV_MAX_SEGS		4

struct umv_queue {
	struct umv_dev *dev;
	u32 index;

	struct vringh vrh;
	struct vring vr;

	void *vring_mem;
	size_t vring_bytes;

	/* 2 * queue_size slots; slot i backs descriptor i. */
	void **slots;
	u32 num_desc;

	int kick_fd;
	int call_fd;
	int irq;

	struct work_struct work;

	/* Diagnostics, dumped by the umvirtio.stats_secs= timer. Plain
	 * counters: this is UP and they only inform. */
	u64 n_kicks;
	u64 n_works;
	u64 n_reqs;
	u64 n_done;
};

/* Storage for one in-flight request, including its iov arrays. */
struct umv_request_priv {
	struct umv_request pub;
	struct kvec riov_store[UMV_MAX_SEGS];
	struct kvec wiov_store[UMV_MAX_SEGS];
};

/* sizeof(struct sockaddr_un.sun_path); not exposed to kernel code. */
#define UMV_SOCK_PATH_MAX	108

static char umv_sock_path[UMV_SOCK_PATH_MAX];
static struct umv_dev *umv_the_dev;

static unsigned int umv_stats_secs;
static struct timer_list umv_stats_timer;

/* ------------------------------------------------------------------ */
/* Queue plumbing                                                      */
/* ------------------------------------------------------------------ */

static void umv_queue_free(struct umv_queue *q)
{
	u32 i;

	if (q->irq >= 0)
		um_free_irq(q->irq, q);

	if (q->slots) {
		for (i = 0; i < q->num_desc; i++)
			if (q->slots[i])
				free_pages_exact(q->slots[i], q->dev->slot_size);
		kfree(q->slots);
		q->slots = NULL;
	}

	if (q->vring_mem) {
		free_pages_exact(q->vring_mem, q->vring_bytes);
		q->vring_mem = NULL;
	}

	umv_user_close(q->kick_fd);
	umv_user_close(q->call_fd);
	q->kick_fd = q->call_fd = q->irq = -1;
}

static void umv_queue_work(struct work_struct *work);

static int umv_queue_alloc(struct umv_dev *dev, struct umv_queue *q, u32 index)
{
	u32 i;

	q->dev = dev;
	q->index = index;
	q->kick_fd = -1;
	q->call_fd = -1;
	q->irq = -1;
	q->num_desc = 2 * dev->queue_size;
	INIT_WORK(&q->work, umv_queue_work);

	q->vring_bytes = PAGE_ALIGN(vring_size(q->num_desc, UMV_VRING_ALIGN));
	q->vring_mem = alloc_pages_exact(q->vring_bytes, GFP_KERNEL | __GFP_ZERO);
	if (!q->vring_mem)
		return -ENOMEM;

	vring_init(&q->vr, q->num_desc, q->vring_mem, UMV_VRING_ALIGN);

	q->slots = kcalloc(q->num_desc, sizeof(*q->slots), GFP_KERNEL);
	if (!q->slots)
		return -ENOMEM;

	/*
	 * Publish each slot's address once. Slots are allocated individually
	 * so no single contiguous allocation has to cover the whole pool;
	 * the host reaches them through its mapping of the physmem fd.
	 */
	for (i = 0; i < q->num_desc; i++) {
		q->slots[i] = alloc_pages_exact(dev->slot_size, GFP_KERNEL);
		if (!q->slots[i])
			return -ENOMEM;

		q->vr.desc[i].addr = cpu_to_le64((u64)(uintptr_t)q->slots[i]);
		q->vr.desc[i].len = cpu_to_le32(dev->slot_size);
		q->vr.desc[i].flags = 0;
		q->vr.desc[i].next = 0;
	}

	q->kick_fd = umv_user_eventfd();
	if (q->kick_fd < 0)
		return q->kick_fd;

	q->call_fd = umv_user_eventfd();
	if (q->call_fd < 0)
		return q->call_fd;

	return 0;
}

static void umv_queue_work(struct work_struct *work)
{
	struct umv_queue *q = container_of(work, struct umv_queue, work);
	struct umv_dev *dev = q->dev;

	q->n_works++;

	for (;;) {
		struct umv_request_priv *p;
		int err;

		p = kzalloc(sizeof(*p), GFP_KERNEL);
		if (!p) {
			/*
			 * Do not return with requests still in the ring: the
			 * host has already kicked for them, so giving up here
			 * would strand them until an unrelated kick.
			 */
			pr_err_ratelimited("queue %u: out of memory\n",
					   q->index);
			msleep(10);
			continue;
		}

		vringh_kiov_init(&p->pub.riov, p->riov_store, UMV_MAX_SEGS);
		vringh_kiov_init(&p->pub.wiov, p->wiov_store, UMV_MAX_SEGS);

		err = vringh_getdesc_kern(&q->vrh, &p->pub.riov, &p->pub.wiov,
					  &p->pub.head, GFP_KERNEL);
		if (err <= 0) {
			kfree(p);
			if (err < 0)
				pr_err("queue %u: getdesc failed: %d\n",
				       q->index, err);
			return;
		}

		q->n_reqs++;
		p->pub.q = q;
		dev->ops->handle(dev, &p->pub);
	}
}

static irqreturn_t umv_kick_irq(int irq, void *data)
{
	struct umv_queue *q = data;

	q->n_kicks++;
	umv_user_eventfd_ack(q->kick_fd);

	/*
	 * Requests can be sitting in the ring before START arrives, since
	 * the host attaches the device to the vDPA bus first. They stay
	 * there; the work is kicked again from umv_start().
	 */
	if (READ_ONCE(q->dev->started))
		schedule_work(&q->work);

	return IRQ_HANDLED;
}

void umv_request_complete(struct umv_request *req, u32 written)
{
	struct umv_request_priv *p =
		container_of(req, struct umv_request_priv, pub);
	struct umv_queue *q = req->q;
	int err;

	err = vringh_complete_kern(&q->vrh, req->head, written);
	if (err < 0)
		pr_err("queue %u: complete failed: %d\n", q->index, err);
	q->n_done++;

	if (vringh_need_notify_kern(&q->vrh) > 0)
		umv_user_eventfd_signal(q->call_fd);

	kfree(p);
}
EXPORT_SYMBOL_GPL(umv_request_complete);

u32 umv_request_queue(const struct umv_request *req)
{
	return req->q->index;
}
EXPORT_SYMBOL_GPL(umv_request_queue);

/* ------------------------------------------------------------------ */
/* Handshake                                                           */
/* ------------------------------------------------------------------ */

static int umv_send_hello(struct umv_dev *dev)
{
	int fds[1 + 2 * UMV_MAX_QUEUES];
	unsigned long long offset;
	struct umv_hello *hello;
	int nfds = 0, rc;
	u32 i;

	hello = kzalloc(sizeof(*hello), GFP_KERNEL);
	if (!hello)
		return -ENOMEM;

	hello->magic = UMV_MAGIC;
	hello->type = UMV_MSG_HELLO;
	hello->version = UMV_VERSION;
	hello->device_id = dev->device_id;
	hello->vendor_id = dev->vendor_id;
	hello->features = dev->features;
	hello->num_queues = dev->num_queues;
	hello->queue_size = dev->queue_size;
	hello->slot_size = dev->slot_size;
	hello->config_size = dev->config_size;
	hello->physmem_size = physmem_size;
	hello->uml_physmem = uml_physmem;
	memcpy(hello->config, dev->config, sizeof(hello->config));

	for (i = 0; i < dev->num_queues; i++)
		hello->vring_kva[i] = (u64)(uintptr_t)dev->queues[i].vring_mem;

	/*
	 * phys_mapping() hands back the fd behind UML's physical memory
	 * along with the offset of the address inside it. Offset 0 is
	 * uml_physmem, which is what the host needs to rebase addresses.
	 */
	rc = phys_mapping(__pa(dev->queues[0].vring_mem), &offset);
	if (rc < 0) {
		pr_err("no physmem mapping for the vring\n");
		kfree(hello);
		return -EFAULT;
	}
	fds[nfds++] = rc;

	for (i = 0; i < dev->num_queues; i++)
		fds[nfds++] = dev->queues[i].kick_fd;
	for (i = 0; i < dev->num_queues; i++)
		fds[nfds++] = dev->queues[i].call_fd;

	rc = umv_user_send_fds(dev->sock_fd, hello, sizeof(*hello), fds, nfds);
	kfree(hello);

	return rc;
}

static int umv_await_start(struct umv_dev *dev)
{
	struct umv_start msg;
	int rc;

	/*
	 * Blocking read in an initcall is fine here: with
	 * CONFIG_UML_NO_USERSPACE there is nothing else for this kernel to
	 * do, and no queue may be touched before the negotiated feature set
	 * is known.
	 */
	rc = umv_user_recv(dev->sock_fd, &msg, sizeof(msg));
	if (rc != sizeof(msg)) {
		pr_err("no START from host: %d\n", rc);
		return rc < 0 ? rc : -EPIPE;
	}

	if (msg.magic != UMV_MAGIC || msg.type != UMV_MSG_START) {
		pr_err("bad START message\n");
		return -EPROTO;
	}

	dev->negotiated = msg.features;
	return 0;
}

int umv_notify_config(struct umv_dev *dev, u32 offset, u32 length)
{
	struct umv_config msg = {
		.magic = UMV_MAGIC,
		.type = UMV_MSG_CONFIG,
		.offset = offset,
		.length = length,
	};

	if (offset > UMV_MAX_CONFIG || length > UMV_MAX_CONFIG - offset)
		return -EINVAL;

	memcpy(msg.data, dev->config + offset, length);

	return umv_user_send(dev->sock_fd, &msg, sizeof(msg));
}
EXPORT_SYMBOL_GPL(umv_notify_config);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/*
 * One line per queue on the console every stats_secs. `avail` is what the
 * bridge has published, `seen` is what this side has consumed, `used` is
 * what it has completed. avail ahead of seen while kicks stand still is a
 * lost kick; seen ahead of used is requests stuck in the shim.
 */
static void umv_stats_fn(struct timer_list *t)
{
	struct umv_dev *dev = umv_the_dev;
	u32 i;

	for (i = 0; i < dev->num_queues; i++) {
		struct umv_queue *q = &dev->queues[i];

		pr_info("q%u: kicks %llu works %llu reqs %llu done %llu | avail %u seen %u used %u\n",
			i, q->n_kicks, q->n_works, q->n_reqs, q->n_done,
			le16_to_cpu(q->vr.avail->idx), q->vrh.last_avail_idx,
			le16_to_cpu(q->vr.used->idx));
	}

	mod_timer(&umv_stats_timer, jiffies + umv_stats_secs * HZ);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

int umv_register_device(const struct umv_device_ops *ops, void *priv)
{
	struct umv_dev *dev;

	if (!ops || !ops->setup || !ops->handle)
		return -EINVAL;
	if (umv_the_dev)
		return -EBUSY;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->ops = ops;
	dev->priv = priv;
	dev->sock_fd = -1;
	dev->sock_irq = -1;
	INIT_LIST_HEAD(&dev->node);

	umv_the_dev = dev;
	return 0;
}
EXPORT_SYMBOL_GPL(umv_register_device);

static void umv_teardown(struct umv_dev *dev)
{
	u32 i;

	if (dev->queues) {
		for (i = 0; i < dev->num_queues; i++)
			umv_queue_free(&dev->queues[i]);
		kfree(dev->queues);
		dev->queues = NULL;
	}

	umv_user_close(dev->sock_fd);
	dev->sock_fd = -1;
}

static int umv_bringup(struct umv_dev *dev)
{
	u32 i;
	int rc;

	rc = dev->ops->setup(dev);
	if (rc < 0) {
		pr_err("%s: setup failed: %d\n", dev->ops->name, rc);
		return rc;
	}

	if (!dev->queue_size)
		dev->queue_size = UMV_DEF_QUEUE_SIZE;
	if (!dev->slot_size)
		dev->slot_size = UMV_DEF_SLOT_SIZE;

	if (dev->num_queues < 1 || dev->num_queues > UMV_MAX_QUEUES ||
	    dev->queue_size < UMV_MIN_QUEUE_SIZE ||
	    dev->queue_size > UMV_MAX_QUEUE_SIZE ||
	    !is_power_of_2(dev->queue_size) ||
	    dev->slot_size > UMV_MAX_SLOT_SIZE ||
	    dev->config_size > UMV_MAX_CONFIG) {
		pr_err("%s: bad geometry\n", dev->ops->name);
		return -EINVAL;
	}

	dev->queues = kcalloc(dev->num_queues, sizeof(*dev->queues),
			      GFP_KERNEL);
	if (!dev->queues)
		return -ENOMEM;

	for (i = 0; i < dev->num_queues; i++) {
		rc = umv_queue_alloc(dev, &dev->queues[i], i);
		if (rc < 0)
			goto err;
	}

	rc = umv_user_connect(umv_sock_path);
	if (rc < 0) {
		pr_err("cannot connect to %s: %d\n", umv_sock_path, rc);
		goto err;
	}
	dev->sock_fd = rc;

	rc = umv_send_hello(dev);
	if (rc < 0) {
		pr_err("hello failed: %d\n", rc);
		goto err;
	}

	rc = umv_await_start(dev);
	if (rc < 0)
		goto err;

	/*
	 * Ring features are fixed (UMV_RING_FEATURES), not dev->negotiated:
	 * the negotiated set describes the device to the host kernel, not
	 * this transport's ring layout. See umvirtio_proto.h.
	 */
	for (i = 0; i < dev->num_queues; i++) {
		struct umv_queue *q = &dev->queues[i];

		rc = vringh_init_kern(&q->vrh, UMV_RING_FEATURES, q->num_desc,
				      false, q->vr.desc, q->vr.avail,
				      q->vr.used);
		if (rc < 0) {
			pr_err("queue %u: vringh init failed: %d\n", i, rc);
			goto err;
		}

		q->irq = um_request_irq(UM_IRQ_ALLOC, q->kick_fd, IRQ_READ,
					umv_kick_irq, 0, "umvirtio", q);
		if (q->irq < 0) {
			rc = q->irq;
			pr_err("queue %u: irq request failed: %d\n", i, rc);
			goto err;
		}

		add_sigio_fd(q->kick_fd);
	}

	if (dev->ops->start) {
		rc = dev->ops->start(dev, dev->negotiated);
		if (rc < 0)
			goto err;
	}

	WRITE_ONCE(dev->started, true);

	/* Drain anything the host queued while we were still negotiating. */
	for (i = 0; i < dev->num_queues; i++)
		schedule_work(&dev->queues[i].work);

	if (umv_stats_secs) {
		timer_setup(&umv_stats_timer, umv_stats_fn, 0);
		mod_timer(&umv_stats_timer, jiffies + umv_stats_secs * HZ);
	}

	pr_info("%s: ready, %u queue(s) of %u, %u byte slots\n",
		dev->ops->name, dev->num_queues, dev->queue_size,
		dev->slot_size);
	return 0;

err:
	umv_teardown(dev);
	return rc;
}

static int __init umv_init(void)
{
	struct umv_dev *dev = umv_the_dev;

	if (!dev)
		return 0;

	if (!umv_sock_path[0]) {
		pr_info("no umvirtio.sock= given, staying idle\n");
		return 0;
	}

	sigio_broken();

	if (umv_bringup(dev) < 0) {
		if (dev->ops->remove)
			dev->ops->remove(dev);
		kfree(dev);
		umv_the_dev = NULL;
	}

	return 0;
}
/*
 * After late_initcall so a shim registering at late_initcall (and any
 * driver it depends on, such as nvme) has already run.
 */
late_initcall_sync(umv_init);

static int umv_sock_set(const char *str, const struct kernel_param *kp)
{
	if (strscpy(umv_sock_path, str, sizeof(umv_sock_path)) < 0)
		return -E2BIG;

	return 0;
}

static int umv_sock_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%s\n", umv_sock_path);
}

static const struct kernel_param_ops umv_sock_param_ops = {
	.set = umv_sock_set,
	.get = umv_sock_get,
};

device_param_cb(sock, &umv_sock_param_ops, NULL, 0400);
__uml_help(umv_sock_param_ops,
"umvirtio.sock=<path>\n"
"    Unix socket the host bridge is listening on. The registered device\n"
"    shim is exported to the host over this socket; without it the shim\n"
"    stays idle.\n\n"
);

module_param_named(stats_secs, umv_stats_secs, uint, 0400);
__uml_help(umv_stats_secs,
"umvirtio.stats_secs=<n>\n"
"    Print per-queue transport counters and ring indices to the console\n"
"    every <n> seconds. 0 (default) disables.\n\n"
);
