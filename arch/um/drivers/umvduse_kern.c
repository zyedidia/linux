// SPDX-License-Identifier: GPL-2.0
/*
 * umvduse: service a VDUSE device (/dev/vduse/$NAME) from the UML kernel.
 *
 * The UML process that runs a real driver is also the VDUSE userspace
 * backend for the virtio device it exports. Everything the host defines
 * for VDUSE happens here: the device is created over /dev/vduse/control,
 * control messages are answered over the device fd, virtqueue kicks
 * arrive as eventfd IRQs, and completions are pushed back with
 * VDUSE_VQ_INJECT_IRQ -- a syscall, not a process wakeup.
 *
 * Memory model. The host driver's buffers are reached through the VDUSE
 * IOVA space. Ring metadata lives in the coherent region (IOVAs above
 * the bounce area) and is mmap'd on demand via VDUSE_IOTLB_GET_FD; those
 * mappings are host memory with no struct page behind them, so they are
 * only ever dereferenced, never fed to anything page-based. Data buffers
 * live in the bounce region [0, bounce_size), which we back with our own
 * vmalloc'd pages via VDUSE_IOTLB_REG_UMEM whenever possible: the host
 * then bounces straight into UML-owned, page-backed memory, and a shim
 * can do zero-copy I/O against it.
 *
 * The rings are run with vringh in kernel mode. vringh hands back raw
 * descriptor addresses (IOVAs), so every kiov entry is translated and
 * bounds-checked here before a shim sees it, and INDIRECT_DESC is never
 * offered (vringh would dereference the indirect table at its IOVA).
 */

#define pr_fmt(fmt) "umvduse: " fmt

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/log2.h>
#include <linux/moduleparam.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/vmalloc.h>
#include <linux/vringh.h>
#include <linux/workqueue.h>

#include <uapi/linux/eventfd.h>
#include <uapi/linux/vduse.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ring.h>

#include <init.h>
#include <irq_kern.h>
#include <os.h>

#include "umvduse.h"
#include "umvduse_user.h"

#define UMVD_VQ_ALIGN		4096
#define UMVD_DEF_UMEM_SIZE	(64 << 20)	/* VDUSE default bounce size */

/*
 * Descriptors covered per direction without a second allocation. A chain
 * may legally span the whole ring, but almost none do: a receive buffer
 * is one descriptor, a command is two, and a transmitted skb is a
 * handful. vringh grows the array itself when a chain overruns it (see
 * resize_iovec()), and vringh_kiov_cleanup() frees what it allocated, so
 * this is a fast-path size and not a limit.
 *
 * Sizing it by the ring instead would put an 8KiB allocation on every
 * request at a 256-entry ring, and 32KiB at 1024 - per packet, on the
 * receive path, for buffers of which one entry is ever used.
 */
#define UMVD_KIOV_INLINE	8

struct umvd_queue {
	struct umvd_dev *dev;
	u32 index;
	u16 num_max;

	/* Dataplane state, valid while num != 0. */
	bool live;
	bool in_work;
	u32 num;
	u32 generation;
	struct vringh vrh;
	u16 saved_avail;

	int kick_fd;
	int irq;
	struct work_struct work;
	/* Serializes ring access: getdesc (work) vs complete (any ctx). */
	spinlock_t lock;
	struct timer_list watchdog;
	bool wdog_stuck;
	u16 wdog_avail, wdog_seen;

	/* Diagnostics, dumped by the umvduse.stats_secs= timer. */
	u64 n_kicks, n_works, n_reqs, n_done, n_notify;
	u64 n_bad, n_dropped, n_watchdog;
};

/* Storage for one in-flight request: kiov arrays sized to the ring, so
 * vringh never needs to grow them (a chain is at most num descriptors). */
struct umvd_request_priv {
	struct umvd_request pub;
	u32 generation;
	struct kvec store[];
};

static char umvd_name[UMVD_NAME_LEN];
static char umvd_umem_param[32];
static unsigned int umvd_stats_secs;

static struct umvd_dev *umvd_the_dev;
static struct timer_list umvd_stats_timer;

static int umvd_ioctl(int fd, unsigned int cmd, void *arg)
{
	return os_ioctl_generic(fd, cmd, (unsigned long)arg);
}

static void umvd_umem_register(struct umvd_dev *dev, bool quiet);

/* ------------------------------------------------------------------ */
/* IOVA -> VA table                                                    */
/* ------------------------------------------------------------------ */

/* Called with iotlb_lock held. */
static struct umvd_iotlb_region *umvd_iotlb_find(struct umvd_dev *dev,
						 u64 iova, u64 len)
{
	int i;

	for (i = 0; i < UMVD_MAX_IOTLB; i++) {
		struct umvd_iotlb_region *r = &dev->iotlb[i];

		if (r->used && iova >= r->start && len <= r->last - iova + 1)
			return r;
	}

	return NULL;
}

/* Called with iotlb_lock held. Fetches and maps the region covering
 * @iova from the host, if there is one. */
static int umvd_iotlb_fetch(struct umvd_dev *dev, u64 iova)
{
	struct vduse_iotlb_entry entry = {
		.start = iova,
		.last = iova,
	};
	struct umvd_iotlb_region *r = NULL;
	int i, fd;
	void *va;

	for (i = 0; i < UMVD_MAX_IOTLB; i++) {
		if (!dev->iotlb[i].used) {
			r = &dev->iotlb[i];
			break;
		}
	}
	if (!r) {
		pr_err("out of IOTLB region slots\n");
		return -ENOSPC;
	}

	fd = umvd_ioctl(dev->dev_fd, VDUSE_IOTLB_GET_FD, &entry);
	if (fd < 0)
		return fd;

	va = umvd_user_mmap(fd, entry.offset, entry.last - entry.start + 1,
			    entry.perm & VDUSE_ACCESS_WO);
	os_close_file(fd);
	if (!va) {
		pr_err("cannot mmap IOTLB region [%llx, %llx]\n",
		       entry.start, entry.last);
		return -ENOMEM;
	}

	r->start = entry.start;
	r->last = entry.last;
	r->va = va;
	r->writable = entry.perm & VDUSE_ACCESS_WO;
	r->used = true;

	return 0;
}

/* Called with iotlb_lock held. */
static void umvd_iotlb_drop_range(struct umvd_dev *dev, u64 start, u64 last)
{
	int i;

	for (i = 0; i < UMVD_MAX_IOTLB; i++) {
		struct umvd_iotlb_region *r = &dev->iotlb[i];

		if (r->used && r->start <= last && start <= r->last) {
			umvd_user_munmap(r->va, r->last - r->start + 1);
			r->used = false;
		}
	}
}

/*
 * Translate an IOVA range to a dereferenceable pointer, mapping the
 * backing region on first use. Returns NULL if the host never told us
 * about the range or it does not fit inside one region. Process context.
 */
static void *umvd_iova_to_va(struct umvd_dev *dev, u64 iova, u64 len,
			     bool need_write)
{
	struct umvd_iotlb_region *r;
	void *va = NULL;

	if (!len || iova + len - 1 < iova)
		return NULL;

	/*
	 * A bounce-range IOVA implies the host has streaming DMA mappings,
	 * which is exactly when a deferred umem registration can succeed.
	 * A persistently failing registration (rlimit, size mismatch) is
	 * retried at most once a second rather than per buffer.
	 */
	if (!dev->umem_live && dev->umem && iova < dev->umem_size &&
	    time_after_eq(jiffies, READ_ONCE(dev->umem_retry_at)))
		umvd_umem_register(dev, false);

	/* The bounce region is our own memory once umem is registered. */
	if (dev->umem_live && len <= dev->umem_size &&
	    iova <= dev->umem_size - len)
		return dev->umem + iova;

	mutex_lock(&dev->iotlb_lock);

	r = umvd_iotlb_find(dev, iova, len);
	if (!r) {
		if (umvd_iotlb_fetch(dev, iova) < 0)
			goto out;
		r = umvd_iotlb_find(dev, iova, len);
		if (!r)
			goto out;
	}

	if (need_write && !r->writable)
		goto out;

	va = r->va + (iova - r->start);
out:
	mutex_unlock(&dev->iotlb_lock);
	return va;
}

static int umvd_translate_kiov(struct umvd_dev *dev, struct vringh_kiov *kiov,
			       bool need_write)
{
	unsigned int i;

	for (i = 0; i < kiov->used; i++) {
		u64 iova = (u64)(uintptr_t)kiov->iov[i].iov_base;
		void *va;

		va = umvd_iova_to_va(dev, iova, kiov->iov[i].iov_len,
				     need_write);
		if (!va)
			return -EFAULT;

		kiov->iov[i].iov_base = va;
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Request path                                                        */
/* ------------------------------------------------------------------ */

/*
 * Queue the drain work on the unbound workqueue: every host fd IRQ
 * lands on CPU0, and schedule_work() would pin the work to CPU0's pool
 * with it. Unbound, an SMP instance drains on an idle CPU while CPU0
 * keeps taking interrupts.
 */
static void umvd_queue_vq_work(struct umvd_queue *q)
{
	queue_work(system_unbound_wq, &q->work);
}

void umvd_request_complete(struct umvd_request *req, u32 written)
{
	struct umvd_request_priv *p =
		container_of(req, struct umvd_request_priv, pub);
	struct umvd_queue *q = req->q;
	struct umvd_dev *dev = q->dev;
	unsigned long flags;
	bool notify = false;
	int err;

	spin_lock_irqsave(&q->lock, flags);
	if (p->generation != q->generation) {
		/* The ring this request came from has been reset. */
		q->n_dropped++;
		spin_unlock_irqrestore(&q->lock, flags);
		goto free;
	}

	err = vringh_complete_kern(&q->vrh, req->head, written);
	if (err < 0)
		pr_err_ratelimited("q%u: complete failed: %d\n", q->index,
				   err);
	q->n_done++;
	notify = vringh_need_notify_kern(&q->vrh) > 0;
	spin_unlock_irqrestore(&q->lock, flags);

	if (notify) {
		q->n_notify++;
		err = umvd_ioctl(dev->dev_fd, VDUSE_VQ_INJECT_IRQ, &q->index);
		if (err < 0)
			pr_err_ratelimited("q%u: inject irq failed: %d\n",
					   q->index, err);
	}

free:
	vringh_kiov_cleanup(&req->riov);
	vringh_kiov_cleanup(&req->wiov);
	kfree(p);
}
EXPORT_SYMBOL_GPL(umvd_request_complete);

u32 umvd_request_queue(const struct umvd_request *req)
{
	return req->q->index;
}
EXPORT_SYMBOL_GPL(umvd_request_queue);

static void umvd_vq_work(struct work_struct *work)
{
	struct umvd_queue *q = container_of(work, struct umvd_queue, work);
	struct umvd_dev *dev = q->dev;

	q->n_works++;
	WRITE_ONCE(q->in_work, true);

	while (READ_ONCE(q->live)) {
		struct umvd_request_priv *p;
		u16 head;
		int err;

		/* kmalloc, not kzalloc: the kiov arrays are written by
		 * vringh before being read (it tracks `used`), and every
		 * header field is assigned below - zeroing would memset
		 * the whole inline area per request for nothing. */
		p = kmalloc(struct_size(p, store, 2 * UMVD_KIOV_INLINE),
			    GFP_KERNEL);
		if (!p) {
			/* The host already kicked for whatever is in the
			 * ring; giving up would strand it until an
			 * unrelated kick. */
			pr_err_ratelimited("q%u: out of memory\n", q->index);
			msleep(10);
			continue;
		}

		vringh_kiov_init(&p->pub.riov, &p->store[0], UMVD_KIOV_INLINE);
		vringh_kiov_init(&p->pub.wiov, &p->store[UMVD_KIOV_INLINE],
				 UMVD_KIOV_INLINE);

		spin_lock_irq(&q->lock);
		err = vringh_getdesc_kern(&q->vrh, &p->pub.riov, &p->pub.wiov,
					  &head, GFP_ATOMIC);
		if (err <= 0) {
			if (err == 0 && !vringh_notify_enable_kern(&q->vrh)) {
				/* More arrived while re-enabling. */
				vringh_notify_disable_kern(&q->vrh);
				spin_unlock_irq(&q->lock);
				kfree(p);
				continue;
			}
			spin_unlock_irq(&q->lock);
			kfree(p);
			if (err < 0)
				pr_err("q%u: getdesc failed: %d\n", q->index,
				       err);
			break;
		}
		p->generation = q->generation;
		spin_unlock_irq(&q->lock);

		p->pub.q = q;
		p->pub.head = head;

		if (umvd_translate_kiov(dev, &p->pub.riov, false) ||
		    umvd_translate_kiov(dev, &p->pub.wiov, true)) {
			q->n_bad++;
			pr_err_ratelimited("q%u: descriptor outside IOTLB\n",
					   q->index);
			umvd_request_complete(&p->pub, 0);
			continue;
		}

		q->n_reqs++;
		dev->ops->handle(dev, &p->pub);
	}

	WRITE_ONCE(q->in_work, false);
}

static irqreturn_t umvd_kick_irq(int irq, void *data)
{
	struct umvd_queue *q = data;
	unsigned long long cnt;

	q->n_kicks++;
	os_read_file(q->kick_fd, &cnt, sizeof(cnt));

	if (READ_ONCE(q->live))
		umvd_queue_vq_work(q);

	return IRQ_HANDLED;
}

/*
 * Backstop for the one lossy edge left in the system (host kick eventfd
 * -> UML IRQ): if avail has moved past what we have seen and no work is
 * queued, a kick may have been dropped. A single observation can also
 * be an instruction-wide benign race (the workqueue clears PENDING just
 * before the work function sets in_work), so only a state still stuck a
 * full period later is counted and rescued -- that keeps n_watchdog a
 * certain lost-kick verdict at the cost of a 2 s worst-case rescue on a
 * path that must never fire at all.
 */
static void umvd_watchdog_fn(struct timer_list *t)
{
	struct umvd_queue *q = timer_container_of(q, t, watchdog);
	u16 avail, seen;

	if (!READ_ONCE(q->live))
		return;

	avail = le16_to_cpu((__force __le16)READ_ONCE(q->vrh.vring.avail->idx));
	seen = q->vrh.last_avail_idx;

	if (avail != seen && !work_pending(&q->work) &&
	    !READ_ONCE(q->in_work)) {
		if (q->wdog_stuck && avail == q->wdog_avail &&
		    seen == q->wdog_seen) {
			q->n_watchdog++;
			q->wdog_stuck = false;
			umvd_queue_vq_work(q);
		} else {
			q->wdog_stuck = true;
			q->wdog_avail = avail;
			q->wdog_seen = seen;
		}
	} else {
		q->wdog_stuck = false;
	}

	mod_timer(&q->watchdog, jiffies + HZ);
}

/* ------------------------------------------------------------------ */
/* umem: back the bounce domain with our own pages                     */
/* ------------------------------------------------------------------ */

static long long umvd_read_bounce_size(struct umvd_dev *dev)
{
	char path[UMVD_NAME_LEN + 40];
	char buf[24];
	int fd, n;

	snprintf(path, sizeof(path), "/sys/class/vduse/%s/bounce_size",
		 dev->name);

	fd = os_open_file(path, of_read(OPENFLAGS()), 0);
	if (fd < 0)
		return fd;

	n = os_read_file(fd, buf, sizeof(buf) - 1);
	os_close_file(fd);
	if (n <= 0)
		return n < 0 ? n : -EIO;

	buf[n] = '\0';
	return simple_strtoull(buf, NULL, 10);
}

/*
 * Make sure the umem buffer exists and matches the device's bounce size,
 * which the launcher may have tuned before attaching us to the vDPA bus.
 */
static int umvd_umem_ensure(struct umvd_dev *dev)
{
	long long size;

	if (umvd_umem_param[0] && !strcmp(umvd_umem_param, "0"))
		return -EPERM;

	size = umvd_read_bounce_size(dev);
	if (size <= 0) {
		pr_warn("cannot read bounce_size (%lld), assuming %u\n",
			size, UMVD_DEF_UMEM_SIZE);
		size = UMVD_DEF_UMEM_SIZE;
	}

	if (dev->umem && dev->umem_size == size)
		return 0;

	/*
	 * Once a registration has pinned the buffer, it must never be
	 * freed or replaced: the host keeps bouncing into the pinned
	 * pages, and handing the shim a different buffer would tear the
	 * two views apart. A bounce_size change after that point can
	 * only come from a torn-down-and-recreated device, which means
	 * a fresh UML too.
	 */
	if (dev->umem_pinned) {
		pr_err("bounce_size changed to %lld under a pinned umem region\n",
		       size);
		return -EBUSY;
	}

	if (dev->umem) {
		vfree(dev->umem);
		dev->umem = NULL;
		dev->umem_size = 0;
	}

	dev->umem = vmalloc(size);
	if (!dev->umem) {
		pr_warn("cannot allocate %lld byte umem region (mem= too small?)\n",
			size);
		return -ENOMEM;
	}

	/* Fault every page in so the host can pin the whole range. */
	memset(dev->umem, 0, size);
	dev->umem_size = size;

	pr_info("umem region: %lld MiB\n", size >> 20);
	return 0;
}

/*
 * Register the umem buffer as the backing for the whole bounce domain.
 *
 * Two host-side subtleties shape this:
 *
 * - The host initializes its bounce machinery on the first streaming
 *   DMA map (vduse_domain_init_bounce_map() runs from
 *   vduse_domain_map_page()), and registration is refused with -EINVAL
 *   before that. At DRIVER_OK no request buffer has been mapped yet, so
 *   the eager attempt there fails on a fresh device; the translation
 *   path retries when the first request arrives, by which time the
 *   host has mapped its buffers. Registering late is safe: the host
 *   migrates in-use bounce pages into the new backing.
 *
 * - The device-level umem record survives a `vdpa dev del`/`add` cycle
 *   even though that replaces the IOVA domain it was registered
 *   against, so -EEXIST may name a registration that backs nothing.
 *   Deregister and register again rather than trusting it.
 */
static void umvd_umem_register(struct umvd_dev *dev, bool quiet)
{
	struct vduse_iova_umem umem = {};
	int err;

	mutex_lock(&dev->iotlb_lock);
	if (dev->umem_live)
		goto out;
	if (umvd_umem_ensure(dev) < 0)
		goto out;

	umem.uaddr = (u64)(uintptr_t)dev->umem;
	umem.iova = 0;
	umem.size = dev->umem_size;

	err = umvd_ioctl(dev->dev_fd, VDUSE_IOTLB_REG_UMEM, &umem);
	if (err == -EEXIST) {
		umvd_ioctl(dev->dev_fd, VDUSE_IOTLB_DEREG_UMEM, &umem);
		err = umvd_ioctl(dev->dev_fd, VDUSE_IOTLB_REG_UMEM, &umem);
	}
	if (err < 0) {
		if (!quiet || err != -EINVAL)
			pr_warn_ratelimited("umem registration failed: %d\n",
					    err);
		if (!quiet)
			WRITE_ONCE(dev->umem_retry_at, jiffies + HZ);
		goto out;
	}

	dev->umem_pinned = true;
	dev->umem_live = true;
	pr_info("umem registered (%zu MiB)\n", dev->umem_size >> 20);
out:
	mutex_unlock(&dev->iotlb_lock);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void umvd_start_dataplane(struct umvd_dev *dev)
{
	bool any = false;
	u64 features = 0;
	u32 i;
	int err;

	err = umvd_ioctl(dev->dev_fd, VDUSE_DEV_GET_FEATURES, &features);
	if (err < 0) {
		pr_err("cannot get negotiated features: %d\n", err);
		return;
	}
	if (!(features & BIT_ULL(VIRTIO_F_VERSION_1))) {
		pr_err("host driver did not accept VERSION_1\n");
		return;
	}
	dev->negotiated = features;

	umvd_umem_register(dev, true);
	if (dev->ops->need_umem && !dev->umem_live)
		pr_info("umem not registered yet; retrying at first request\n");

	for (i = 0; i < dev->num_queues; i++) {
		struct umvd_queue *q = &dev->queues[i];
		struct vduse_vq_info info = { .index = i };
		struct vduse_vq_eventfd vq_efd = { .index = i };
		bool event = features & BIT_ULL(VIRTIO_RING_F_EVENT_IDX);
		void *desc, *avail, *used;

		err = umvd_ioctl(dev->dev_fd, VDUSE_VQ_GET_INFO, &info);
		if (err < 0) {
			pr_err("q%u: cannot get info: %d\n", i, err);
			return;
		}
		if (!info.ready || !info.num)
			continue;

		/*
		 * The host drops kick eventfd registrations on every
		 * device reset (vduse_dev_reset()), and the driver
		 * resets the device on its way to DRIVER_OK, so this is
		 * the only registration point that sticks. A kick that
		 * arrived before we got here is replayed by the host on
		 * registration (the vq->kicked flag).
		 */
		vq_efd.fd = q->kick_fd;
		err = umvd_ioctl(dev->dev_fd, VDUSE_VQ_SETUP_KICKFD, &vq_efd);
		if (err < 0) {
			pr_err("q%u: kickfd setup failed: %d\n", i, err);
			return;
		}

		desc = umvd_iova_to_va(dev, info.desc_addr,
				       (u64)info.num * sizeof(struct vring_desc),
				       false);
		avail = umvd_iova_to_va(dev, info.driver_addr,
					4 + 2 * (u64)info.num + (event ? 2 : 0),
					false);
		used = umvd_iova_to_va(dev, info.device_addr,
				       4 + 8 * (u64)info.num + (event ? 2 : 0),
				       true);
		if (!desc || !avail || !used) {
			pr_err("q%u: cannot map ring (desc %llx avail %llx used %llx)\n",
			       i, info.desc_addr, info.driver_addr,
			       info.device_addr);
			return;
		}

		err = vringh_init_kern(&q->vrh, features, info.num, false,
				       desc, avail, used);
		if (err < 0) {
			pr_err("q%u: vringh init failed: %d\n", i, err);
			return;
		}
		q->vrh.last_avail_idx = info.split.avail_index;
		q->num = info.num;
		any = true;
	}

	if (!any) {
		pr_warn("DRIVER_OK with no ready queue\n");
		return;
	}

	if (dev->ops->start) {
		err = dev->ops->start(dev, features);
		if (err < 0) {
			pr_err("%s: start failed: %d\n", dev->ops->name, err);
			return;
		}
	}

	dev->started = true;
	for (i = 0; i < dev->num_queues; i++) {
		struct umvd_queue *q = &dev->queues[i];

		if (!q->num)
			continue;
		WRITE_ONCE(q->live, true);
		/* Drain anything posted before we went live. */
		umvd_queue_vq_work(q);
		q->wdog_stuck = false;
		mod_timer(&q->watchdog, jiffies + HZ);
	}

	pr_info("started: features 0x%llx%s\n", features,
		dev->umem_live ? ", umem live" : "");
}

static void umvd_reset(struct umvd_dev *dev)
{
	u32 i;

	if (dev->started) {
		dev->started = false;

		for (i = 0; i < dev->num_queues; i++)
			WRITE_ONCE(dev->queues[i].live, false);

		for (i = 0; i < dev->num_queues; i++) {
			struct umvd_queue *q = &dev->queues[i];

			timer_delete_sync(&q->watchdog);
			cancel_work_sync(&q->work);

			if (!q->num)
				continue;
			spin_lock_irq(&q->lock);
			q->saved_avail = q->vrh.last_avail_idx;
			q->generation++;
			q->num = 0;
			spin_unlock_irq(&q->lock);
		}

		if (dev->ops->reset)
			dev->ops->reset(dev);
	}

	/*
	 * Drop every cached IOTLB mapping and mark the umem registration
	 * stale, even if the dataplane never started (a half-failed start
	 * can leave mappings behind): a vdpa del/add cycle replaces the
	 * IOVA domain, so anything cached across it would point at the
	 * old domain's file and the old registration would back nothing.
	 * Remaps are lazy, re-registration happens on the next start, and
	 * the umem buffer itself is our own memory and stays.
	 */
	mutex_lock(&dev->iotlb_lock);
	umvd_iotlb_drop_range(dev, 0, U64_MAX);
	dev->umem_live = false;
	WRITE_ONCE(dev->umem_retry_at, 0);
	mutex_unlock(&dev->iotlb_lock);

	pr_info("reset\n");
}

/* ------------------------------------------------------------------ */
/* Message loop                                                        */
/* ------------------------------------------------------------------ */

static void umvd_handle_message(struct umvd_dev *dev,
				struct vduse_dev_request *req,
				struct vduse_dev_response *resp)
{
	resp->request_id = req->request_id;
	resp->result = VDUSE_REQ_RESULT_OK;

	switch (req->type) {
	case VDUSE_GET_VQ_STATE: {
		u32 index = req->vq_state.index;
		struct umvd_queue *q;

		if (index >= dev->num_queues) {
			resp->result = VDUSE_REQ_RESULT_FAILED;
			break;
		}
		q = &dev->queues[index];
		resp->vq_state.index = index;
		resp->vq_state.split.avail_index =
			q->num ? q->vrh.last_avail_idx : q->saved_avail;
		break;
	}
	case VDUSE_SET_STATUS: {
		u8 status = req->s.status;
		u8 old = dev->status;

		dev->status = status;
		if (status == 0)
			umvd_reset(dev);
		else if ((status & VIRTIO_CONFIG_S_DRIVER_OK) &&
			 !(old & VIRTIO_CONFIG_S_DRIVER_OK) && !dev->started)
			umvd_start_dataplane(dev);
		break;
	}
	case VDUSE_UPDATE_IOTLB: {
		u32 i;

		/*
		 * Never sent for the static bounce domain virtio_vdpa
		 * uses, but must be honored: quiesce the queue works,
		 * drop the overlapping mappings, let translation
		 * re-fault them.
		 */
		for (i = 0; i < dev->num_queues; i++)
			cancel_work_sync(&dev->queues[i].work);

		mutex_lock(&dev->iotlb_lock);
		umvd_iotlb_drop_range(dev, req->iova.start, req->iova.last);
		mutex_unlock(&dev->iotlb_lock);

		for (i = 0; i < dev->num_queues; i++)
			if (READ_ONCE(dev->queues[i].live))
				umvd_queue_vq_work(&dev->queues[i]);

		pr_info("iotlb update [%llx, %llx]\n", req->iova.start,
			req->iova.last);
		break;
	}
	default:
		pr_warn("unknown message type %u\n", req->type);
		resp->result = VDUSE_REQ_RESULT_FAILED;
		break;
	}
}

static void umvd_msg_work(struct work_struct *work)
{
	struct umvd_dev *dev = container_of(work, struct umvd_dev, msg_work);

	for (;;) {
		struct vduse_dev_request req;
		struct vduse_dev_response resp = {};
		int n;

		n = os_read_file(dev->dev_fd, &req, sizeof(req));
		if (n == -EAGAIN || n == 0)
			return;
		if (n < 0) {
			pr_err_ratelimited("message read failed: %d\n", n);
			return;
		}
		if (n != sizeof(req)) {
			pr_err("short message read: %d\n", n);
			return;
		}

		umvd_handle_message(dev, &req, &resp);

		n = os_write_file(dev->dev_fd, &resp, sizeof(resp));
		if (n != sizeof(resp))
			pr_err("message reply failed: %d\n", n);
	}
}

static irqreturn_t umvd_msg_irq(int irq, void *data)
{
	struct umvd_dev *dev = data;

	schedule_work(&dev->msg_work);
	return IRQ_HANDLED;
}

/* ------------------------------------------------------------------ */
/* Config updates                                                      */
/* ------------------------------------------------------------------ */

int umvd_notify_config(struct umvd_dev *dev, u32 offset, u32 length)
{
	struct vduse_config_data *data;
	int err;

	if (offset > dev->config_size || length > dev->config_size - offset)
		return -EINVAL;

	data = kzalloc(struct_size(data, buffer, length), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->offset = offset;
	data->length = length;
	memcpy(data->buffer, dev->config + offset, length);

	err = umvd_ioctl(dev->dev_fd, VDUSE_DEV_SET_CONFIG, data);
	kfree(data);
	if (err < 0)
		return err;

	return umvd_ioctl(dev->dev_fd, VDUSE_DEV_INJECT_CONFIG_IRQ, NULL);
}
EXPORT_SYMBOL_GPL(umvd_notify_config);

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

static void umvd_stats_fn(struct timer_list *t)
{
	struct umvd_dev *dev = umvd_the_dev;
	u32 i;

	for (i = 0; i < dev->num_queues; i++) {
		struct umvd_queue *q = &dev->queues[i];
		bool active;
		u16 avail = 0, seen = 0;
		unsigned long flags;

		/* The ring pointers die on reset; sample them under the
		 * queue lock, which reset takes before retiring them. */
		spin_lock_irqsave(&q->lock, flags);
		active = q->num != 0;
		if (active) {
			avail = le16_to_cpu((__force __le16)
				READ_ONCE(q->vrh.vring.avail->idx));
			seen = q->vrh.last_avail_idx;
		}
		spin_unlock_irqrestore(&q->lock, flags);

		if (active)
			pr_info("q%u: kicks %llu works %llu reqs %llu done %llu notify %llu bad %llu dropped %llu wdog %llu | avail %u seen %u\n",
				i, q->n_kicks, q->n_works, q->n_reqs,
				q->n_done, q->n_notify, q->n_bad,
				q->n_dropped, q->n_watchdog, avail, seen);
		else
			pr_info("q%u: kicks %llu works %llu reqs %llu done %llu notify %llu bad %llu dropped %llu wdog %llu | inactive\n",
				i, q->n_kicks, q->n_works, q->n_reqs,
				q->n_done, q->n_notify, q->n_bad,
				q->n_dropped, q->n_watchdog);
	}

	mod_timer(&umvd_stats_timer, jiffies + umvd_stats_secs * HZ);
}

/* ------------------------------------------------------------------ */
/* Bring-up                                                            */
/* ------------------------------------------------------------------ */

int umvd_register_device(const struct umvd_device_ops *ops, void *priv)
{
	struct umvd_dev *dev;

	if (!ops || !ops->setup || !ops->handle)
		return -EINVAL;
	if (umvd_the_dev)
		return -EBUSY;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->ops = ops;
	dev->priv = priv;
	dev->ctrl_fd = -1;
	dev->dev_fd = -1;
	dev->dev_irq = -1;
	mutex_init(&dev->iotlb_lock);
	INIT_WORK(&dev->msg_work, umvd_msg_work);

	umvd_the_dev = dev;
	return 0;
}
EXPORT_SYMBOL_GPL(umvd_register_device);

static int umvd_create_device(struct umvd_dev *dev)
{
	struct vduse_dev_config *cfg;
	char *name;
	u64 api = 0;
	int err;

	dev->ctrl_fd = os_open_file("/dev/vduse/control",
				    of_rdwr(OPENFLAGS()), 0);
	if (dev->ctrl_fd < 0) {
		pr_err("cannot open /dev/vduse/control: %d (is the host vduse module loaded?)\n",
		       dev->ctrl_fd);
		return dev->ctrl_fd;
	}

	err = umvd_ioctl(dev->ctrl_fd, VDUSE_SET_API_VERSION, &api);
	if (err < 0) {
		pr_err("cannot set API version 0: %d\n", err);
		return err;
	}

	/* Crash-leftover devices persist by design; clear ours if it is
	 * still around from a previous run. */
	name = kzalloc(VDUSE_NAME_MAX, GFP_KERNEL);
	if (!name)
		return -ENOMEM;
	strscpy(name, dev->name, VDUSE_NAME_MAX);
	err = umvd_ioctl(dev->ctrl_fd, VDUSE_DESTROY_DEV, name);
	if (err == 0)
		pr_info("destroyed leftover /dev/vduse/%s\n", dev->name);
	kfree(name);

	cfg = kzalloc(struct_size(cfg, config, dev->config_size), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	strscpy(cfg->name, dev->name, VDUSE_NAME_MAX);
	cfg->vendor_id = dev->vendor_id;
	cfg->device_id = dev->device_id;
	cfg->features = dev->features;
	cfg->vq_num = dev->num_queues;
	cfg->vq_align = UMVD_VQ_ALIGN;
	cfg->config_size = dev->config_size;
	memcpy(cfg->config, dev->config, dev->config_size);

	err = umvd_ioctl(dev->ctrl_fd, VDUSE_CREATE_DEV, cfg);
	kfree(cfg);
	if (err < 0) {
		pr_err("cannot create /dev/vduse/%s: %d\n", dev->name, err);
		return err;
	}

	return 0;
}

static void umvd_teardown(struct umvd_dev *dev)
{
	u32 i;

	if (dev->dev_irq >= 0) {
		ignore_sigio_fd(dev->dev_fd);
		um_free_irq(dev->dev_irq, dev);
		dev->dev_irq = -1;
	}
	cancel_work_sync(&dev->msg_work);

	if (dev->queues) {
		for (i = 0; i < dev->num_queues; i++) {
			struct umvd_queue *q = &dev->queues[i];

			if (q->irq >= 0) {
				ignore_sigio_fd(q->kick_fd);
				um_free_irq(q->irq, q);
			}
			if (q->kick_fd >= 0)
				os_close_file(q->kick_fd);
		}
		kfree(dev->queues);
		dev->queues = NULL;
	}

	if (dev->dev_fd >= 0) {
		os_close_file(dev->dev_fd);
		dev->dev_fd = -1;
	}

	if (dev->ctrl_fd >= 0) {
		char name[UMVD_NAME_LEN];

		strscpy(name, dev->name, sizeof(name));
		umvd_ioctl(dev->ctrl_fd, VDUSE_DESTROY_DEV, name);
		os_close_file(dev->ctrl_fd);
		dev->ctrl_fd = -1;
	}

	if (dev->umem) {
		vfree(dev->umem);
		dev->umem = NULL;
		dev->umem_size = 0;
	}
}

static int umvd_bringup(struct umvd_dev *dev)
{
	char path[UMVD_NAME_LEN + 16];
	u32 i;
	unsigned long long memlock = ~0ULL;
	int err;

	err = dev->ops->setup(dev);
	if (err < 0) {
		pr_err("%s: setup failed: %d\n", dev->ops->name, err);
		return err;
	}

	/* Transport-owned bits. INDIRECT_DESC is never offered: vringh in
	 * kernel mode would dereference the indirect table at its IOVA. */
	dev->features |= BIT_ULL(VIRTIO_F_VERSION_1) |
			 BIT_ULL(VIRTIO_F_ACCESS_PLATFORM) |
			 BIT_ULL(VIRTIO_RING_F_EVENT_IDX);

	if (!dev->device_id || dev->num_queues < 1 ||
	    dev->num_queues > UMVD_MAX_QUEUES ||
	    dev->config_size > UMVD_MAX_CONFIG) {
		pr_err("%s: bad identity\n", dev->ops->name);
		return -EINVAL;
	}
	for (i = 0; i < dev->num_queues; i++) {
		if (dev->queue_max[i] < 4 || dev->queue_max[i] > 1024 ||
		    !is_power_of_2(dev->queue_max[i])) {
			pr_err("%s: bad queue %u size %u\n", dev->ops->name,
			       i, dev->queue_max[i]);
			return -EINVAL;
		}
	}

	if (!umvd_name[0])
		strscpy(umvd_name, dev->ops->name, sizeof(umvd_name));
	strscpy(dev->name, umvd_name, sizeof(dev->name));

	/*
	 * REG_UMEM checks the whole bounce-region pin against
	 * RLIMIT_MEMLOCK and, unlike VFIO, grants no CAP_IPC_LOCK
	 * exemption -- the inherited default (~8 MiB) is far below any
	 * bounce size. We run with CAP_SYS_RESOURCE; lift the limit
	 * rather than make every launcher remember to.
	 */
	err = umvd_user_raise_memlock(&memlock);
	if (err < 0)
		pr_warn("cannot raise RLIMIT_MEMLOCK: %d; umem registration may fail\n",
			err);
	else if (memlock != ~0ULL)
		pr_info("RLIMIT_MEMLOCK raised to %llu MiB (the inherited hard limit); a larger bounce region will not register\n",
			memlock >> 20);

	err = umvd_create_device(dev);
	if (err < 0)
		return err;

	snprintf(path, sizeof(path), "/dev/vduse/%s", dev->name);
	dev->dev_fd = os_open_file(path, of_rdwr(OPENFLAGS()), 0);
	if (dev->dev_fd < 0) {
		pr_err("cannot open %s: %d\n", path, dev->dev_fd);
		return dev->dev_fd;
	}
	err = os_set_fd_block(dev->dev_fd, 0);
	if (err < 0)
		return err;

	dev->queues = kcalloc(dev->num_queues, sizeof(*dev->queues),
			      GFP_KERNEL);
	if (!dev->queues)
		return -ENOMEM;

	for (i = 0; i < dev->num_queues; i++) {
		struct umvd_queue *q = &dev->queues[i];
		struct vduse_vq_config vq_cfg = {
			.index = i,
			.max_size = dev->queue_max[i],
		};

		q->dev = dev;
		q->index = i;
		q->num_max = dev->queue_max[i];
		q->kick_fd = -1;
		q->irq = -1;
		spin_lock_init(&q->lock);
		INIT_WORK(&q->work, umvd_vq_work);
		timer_setup(&q->watchdog, umvd_watchdog_fn, 0);

		err = umvd_ioctl(dev->dev_fd, VDUSE_VQ_SETUP, &vq_cfg);
		if (err < 0) {
			pr_err("q%u: setup failed: %d\n", i, err);
			return err;
		}

		/* Registered with the host at DRIVER_OK, not here: reset
		 * wipes kickfd registrations and the driver always resets
		 * before it gets there. */
		q->kick_fd = os_eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (q->kick_fd < 0)
			return q->kick_fd;

		q->irq = um_request_irq(UM_IRQ_ALLOC, q->kick_fd, IRQ_READ,
					umvd_kick_irq, 0, "umvduse", q);
		if (q->irq < 0) {
			pr_err("q%u: irq request failed: %d\n", i, q->irq);
			return q->irq;
		}
		err = add_sigio_fd(q->kick_fd);
		if (err < 0)
			return err;
	}

	/*
	 * After the VQ_SETUP loop, so the window between the device node
	 * appearing and the device being attachable (vduse_dev_is_ready()
	 * wants every vq configured) is not widened by the 64 MiB
	 * allocate-and-touch. Allocating up front still means a hopeless
	 * configuration (mem= too small for the bounce size, umem=0 with
	 * a shim that needs it) fails at boot rather than at first I/O;
	 * registration itself waits for the host's bounce machinery.
	 */
	err = umvd_umem_ensure(dev);
	if (err < 0 && dev->ops->need_umem) {
		pr_err("%s needs the umem region\n", dev->ops->name);
		return err;
	}

	dev->dev_irq = um_request_irq(UM_IRQ_ALLOC, dev->dev_fd, IRQ_READ,
				      umvd_msg_irq, 0, "umvduse-msg", dev);
	if (dev->dev_irq < 0) {
		pr_err("message irq request failed: %d\n", dev->dev_irq);
		return dev->dev_irq;
	}
	err = add_sigio_fd(dev->dev_fd);
	if (err < 0)
		return err;

	if (umvd_stats_secs) {
		timer_setup(&umvd_stats_timer, umvd_stats_fn, 0);
		mod_timer(&umvd_stats_timer, jiffies + umvd_stats_secs * HZ);
	}

	/* Drain anything that arrived before the IRQ was wired up. */
	schedule_work(&dev->msg_work);

	pr_info("/dev/vduse/%s ready: %s, device id %u, %u queue(s)\n",
		dev->name, dev->ops->name, dev->device_id, dev->num_queues);
	return 0;
}

static int __init umvd_init(void)
{
	struct umvd_dev *dev = umvd_the_dev;

	if (!dev)
		return 0;

	sigio_broken();

	if (umvd_bringup(dev) < 0) {
		umvd_teardown(dev);
		if (dev->ops->remove)
			dev->ops->remove(dev);
		kfree(dev);
		umvd_the_dev = NULL;
	}

	return 0;
}
/*
 * After late_initcall so a shim registering at late_initcall (and any
 * driver it depends on, such as nvme) has already run.
 */
late_initcall_sync(umvd_init);

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

module_param_string(name, umvd_name, sizeof(umvd_name), 0400);
__uml_help(umvd_name,
"umvduse.name=<name>\n"
"    Name of the VDUSE device to create (/dev/vduse/<name>). Defaults\n"
"    to the name of the registered device shim.\n\n"
);

module_param_string(umem, umvd_umem_param, sizeof(umvd_umem_param), 0400);
__uml_help(umvd_umem_param,
"umvduse.umem=0\n"
"    Do not register UML memory as the backing for the VDUSE bounce\n"
"    domain. Devices that need zero-copy I/O (umvduse-blk) will refuse\n"
"    to start. By default the region is sized to the device's\n"
"    bounce_size (64 MiB unless tuned via sysfs) and comes out of\n"
"    UML's memory, so mem= must have room for it.\n\n"
);

module_param_named(stats_secs, umvd_stats_secs, uint, 0400);
__uml_help(umvd_stats_secs,
"umvduse.stats_secs=<n>\n"
"    Print per-queue transport counters and ring indices to the console\n"
"    every <n> seconds. 0 (default) disables.\n\n"
);
