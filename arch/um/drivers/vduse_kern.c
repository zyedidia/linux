// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-uml: " fmt

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/kstrtox.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ring.h>
#include <irq_kern.h>
#include <init.h>
#include <mem.h>
#include <os.h>

#include "vduse.h"
#include "vduse_user.h"

#define UML_VDUSE_MAX_DEVICES 8
#define UML_VDUSE_LOG_MAGIC 0x676f6c76
#define UML_VDUSE_LOG_VERSION 1

/*
 * The in-flight log lives in a host file that outlives the guest, so that
 * the next guest can tell which requests were outstanding. A header is
 * followed by one area per queue, with an entry for each descriptor. The
 * header names the device by the host inode of /dev/vduse/<name>, which is
 * new each time the device is created.
 */
struct uml_vduse_log_header {
	u32 magic;
	u32 version;
	u32 num_queues;
	u32 queue_size;
	u64 node_dev;
	u64 node_ino;
};

struct uml_vduse_log_desc {
	u8 inflight;
	u8 pad[7];
	/* The order in which the requests were taken. */
	u64 counter;
};

struct uml_vduse_log_queue {
	/* The used index and the head of the last completion. */
	u16 used_idx;
	u16 last_head;
	u8 pad[12];
	struct uml_vduse_log_desc desc[];
};

struct uml_vduse_region {
	struct uml_vduse_user_region user;
	struct list_head list;
};

/*
 * Counters of a queue. Kicks is the sum of the kick eventfd counts read, so
 * it counts the host driver's notifications, and kick_irqs counts the runs of
 * the kick handler. Requests and out_bytes count the chains taken and the
 * sizes of their readable buffers. In_bytes is the sum of the used lengths.
 * Each completion either injects an interrupt or is suppressed because the
 * driver does not want one.
 */
struct uml_vduse_vq_stats {
	atomic64_t kicks;
	atomic64_t kick_irqs;
	atomic64_t dropped_kicks;
	atomic64_t requests;
	atomic64_t bad_requests;
	atomic64_t out_bytes;
	atomic64_t in_bytes;
	atomic64_t injected;
	atomic64_t suppressed;
	atomic64_t inject_errors;
	atomic64_t replayed;
	atomic64_t polled;
	atomic64_t poll_timeouts;
};

struct uml_vduse_dev_stats {
	atomic64_t msgs[UML_VDUSE_MSG_UNKNOWN + 1];
	atomic64_t starts;
	atomic64_t resets;
	atomic64_t maps;
	atomic64_t unmaps;
	atomic64_t map_errors;
	atomic64_t reconnects;
	atomic64_t umem_regs;
};

struct uml_vduse_vq {
	struct uml_vduse_dev *dev;
	unsigned int index;
	int kickfd;
	int irq;
	struct vringh vrh;
	/*
	 * Protects the used ring and the log. The available ring is only
	 * accessed from the backend's processing of the queue.
	 */
	spinlock_t lock;
	bool ready;
	bool running;

	/* The queue's area of the in-flight log. */
	struct uml_vduse_log_queue *log;
	u64 counter;
	/* Requests of an earlier guest to take again, oldest first. */
	u16 *replay;
	unsigned int nr_replay;
	unsigned int replay_pos;

	struct uml_vduse_vq_stats stats;
};

struct uml_vduse_dev {
	char *type;
	char *name;
	/* The options left for the backend. */
	char *opts;

	const struct uml_vduse_backend_ops *ops;
	void *priv;

	int fd;
	int irq;
	struct work_struct msg_work;

	u16 queue_size;
	unsigned int num_queues;
	struct uml_vduse_vq *vqs;

	/* The features that the host's driver accepted. */
	u64 features;
	bool running;
	/*
	 * How long a queue that runs out of buffers waits for more before it
	 * enables notifications, set by poll=<usecs>.
	 */
	u64 poll_ns;
	/*
	 * Reconnect is set when the device was left by a guest, until a start
	 * takes over its queues. Resume is set until the message work has
	 * tried to.
	 */
	bool reconnect;
	bool resume;

	/* The in-flight log and the file given for it. */
	char *log_path;
	int log_fd;
	void *log;
	size_t log_size;

	/* Regions of the IOVA space mapped into UML, protected by map_lock. */
	struct list_head regions;
	struct mutex map_lock;

	/*
	 * Guest pages that back the bounce region, so that backends get guest
	 * pages for the host's buffers. Chunks of contiguous pages lie side by
	 * side in a host window, which is registered with VDUSE. Umem_lock
	 * serializes the registration, and umem_wanted asks for one at the
	 * next translation. Bounce_guest is set by bounce=guest, without which
	 * the region stays in host memory.
	 */
	bool bounce_guest;
	struct mutex umem_lock;
	struct page **umem_chunks;
	unsigned int umem_nr_chunks;
	unsigned int umem_shift;
	void *umem_window;
	u64 umem_size;
	bool umem_wanted;
	bool umem_registered;

	struct uml_vduse_dev_stats stats;
	struct list_head list;
};

static const struct uml_vduse_backend_ops *const uml_vduse_backends[] = {
#ifdef CONFIG_UML_VDUSE_BLK
	&uml_vduse_blk_ops,
#endif
};

static const char *uml_vduse_args[UML_VDUSE_MAX_DEVICES];
static int uml_vduse_nr_args;
static int uml_vduse_control = -1;

/* The devices that were created, which is fixed after boot. */
static LIST_HEAD(uml_vduse_devices);

const char *uml_vduse_name(struct uml_vduse_dev *dev)
{
	return dev->name;
}

/*
 * Find the region that holds an IOVA, and map it on a miss. Regions are only
 * unmapped while the queues are stopped, so a region stays valid after the
 * lock is dropped.
 */
static struct uml_vduse_region *uml_vduse_find_region(struct uml_vduse_dev *dev,
						      u64 iova)
{
	struct uml_vduse_region *r;
	int err;

	guard(mutex)(&dev->map_lock);

	list_for_each_entry(r, &dev->regions, list) {
		if (iova >= r->user.start && iova <= r->user.last)
			return r;
	}

	r = kzalloc_obj(*r);
	if (!r)
		return NULL;

	err = uml_vduse_user_map(dev->fd, iova, &r->user);
	if (err) {
		atomic64_inc(&dev->stats.map_errors);
		pr_warn_ratelimited("%s: cannot map IOVA 0x%llx: %d\n",
				    dev->name, iova, err);
		kfree(r);
		return NULL;
	}
	list_add(&r->list, &dev->regions);
	atomic64_inc(&dev->stats.maps);
	return r;
}

static void *uml_vduse_translate(struct uml_vduse_dev *dev, u64 iova,
				 size_t len, bool write)
{
	struct uml_vduse_region *r;
	u64 last;

	if (!len || check_add_overflow(iova, (u64)len - 1, &last))
		return NULL;

	r = uml_vduse_find_region(dev, iova);
	if (!r || last > r->user.last)
		return NULL;
	if (write ? !r->user.writable : !r->user.readable)
		return NULL;
	return r->user.addr + (iova - r->user.start);
}

static void uml_vduse_free_umem(struct uml_vduse_dev *dev)
{
	unsigned int i, order = dev->umem_shift - PAGE_SHIFT;

	if (dev->umem_window)
		uml_vduse_user_unmap_window(dev->umem_window, dev->umem_size);
	for (i = 0; i < dev->umem_nr_chunks; i++) {
		if (dev->umem_chunks[i])
			__free_pages(dev->umem_chunks[i], order);
	}
	kfree(dev->umem_chunks);
	dev->umem_chunks = NULL;
	dev->umem_nr_chunks = 0;
	dev->umem_window = NULL;
	dev->umem_size = 0;
}

/*
 * Allocate guest pages for a bounce region of the given size, in chunks of
 * the largest order the page allocator gives, and map them side by side in
 * a host window. The chunks are compound pages, so that a reference taken on
 * any of their pages is counted.
 */
static int uml_vduse_alloc_umem(struct uml_vduse_dev *dev, u64 size)
{
	unsigned int i, nr, order = min(get_order(size), MAX_PAGE_ORDER);
	u64 chunk = PAGE_SIZE << order, off;
	unsigned long long phys;
	int fd, err;

	if (!PAGE_ALIGNED(size))
		return -EINVAL;

	uml_vduse_free_umem(dev);
	dev->umem_shift = PAGE_SHIFT + order;
	nr = DIV_ROUND_UP(size, chunk);
	dev->umem_chunks = kzalloc_objs(*dev->umem_chunks, nr);
	if (!dev->umem_chunks)
		goto nomem;
	dev->umem_nr_chunks = nr;

	dev->umem_window = uml_vduse_user_reserve_window(size);
	if (!dev->umem_window)
		goto nomem;
	dev->umem_size = size;

	for (i = 0, off = 0; i < dev->umem_nr_chunks; i++, off += chunk) {
		dev->umem_chunks[i] = alloc_pages(GFP_KERNEL | __GFP_NOWARN |
						  __GFP_COMP, order);
		if (!dev->umem_chunks[i])
			goto nomem;

		fd = phys_mapping(page_to_phys(dev->umem_chunks[i]), &phys);
		err = uml_vduse_user_map_window(dev->umem_window + off,
						min(chunk, size - off), fd, phys);
		if (err) {
			uml_vduse_free_umem(dev);
			return err;
		}
	}

	return 0;

nomem:
	uml_vduse_free_umem(dev);
	return -ENOMEM;
}

/*
 * Back the bounce region with guest pages. VDUSE accepts this only after
 * the host's first streaming map, so a start asks for it and every
 * translation tries until VDUSE reports the region. The host makes its
 * streaming maps before it publishes a chain that uses them, so the first
 * translation of an address in the region always succeeds in registering.
 * A registration may be left from an earlier start, or from a domain that
 * the host has since replaced, so it is dropped first. Any other failure
 * leaves the region in host memory, and the backends copy.
 */
static void uml_vduse_register_umem(struct uml_vduse_dev *dev)
{
	u64 size;
	int err;

	guard(mutex)(&dev->umem_lock);
	if (!dev->umem_wanted)
		return;

	err = uml_vduse_user_bounce_size(dev->fd, &size);
	if (err == -EINVAL)
		return;

	if (!err && size != dev->umem_size)
		err = uml_vduse_alloc_umem(dev, size);
	if (!err) {
		uml_vduse_user_dereg_umem(dev->fd, size);
		err = uml_vduse_user_reg_umem(dev->fd, dev->umem_window, size);
	}
	if (!err) {
		dev->umem_registered = true;
		atomic64_inc(&dev->stats.umem_regs);
	} else {
		pr_warn("%s: the bounce region stays in host memory, so requests are copied: %d\n",
			dev->name, err);
	}

	/*
	 * Another queue that finds no registration wanted must also find its
	 * result, so the request is withdrawn last.
	 */
	smp_store_release(&dev->umem_wanted, false);
}

/*
 * Give vringh the region that holds a buffer or an indirect table. The host
 * maps the bounce region and coherent allocations for both reading and
 * writing, so a range covers a whole region.
 */
static bool uml_vduse_getrange(struct vringh *vrh, u64 addr,
			       struct vringh_range *range)
{
	struct uml_vduse_vq *vq = container_of(vrh, struct uml_vduse_vq, vrh);
	struct uml_vduse_dev *dev = vq->dev;
	struct uml_vduse_region *r;

	if (smp_load_acquire(&dev->umem_wanted))
		uml_vduse_register_umem(dev);

	/* With guest pages behind it, the bounce region is one range a chunk. */
	if (dev->umem_registered && addr < dev->umem_size) {
		u64 chunk = addr >> dev->umem_shift;

		range->start = chunk << dev->umem_shift;
		range->end_incl = min(range->start + BIT_ULL(dev->umem_shift),
				      dev->umem_size) - 1;
		range->offset = (unsigned long)
			page_address(dev->umem_chunks[chunk]) - range->start;
		return true;
	}

	r = uml_vduse_find_region(dev, addr);
	if (!r)
		return false;

	range->start = r->user.start;
	range->end_incl = r->user.last;
	range->offset = (unsigned long)r->user.addr - r->user.start;
	return true;
}

static void uml_vduse_unmap_range(struct uml_vduse_dev *dev, u64 start,
				  u64 last)
{
	struct uml_vduse_region *r, *n;

	guard(mutex)(&dev->map_lock);

	list_for_each_entry_safe(r, n, &dev->regions, list) {
		if (r->user.last < start || r->user.start > last)
			continue;
		uml_vduse_user_unmap(&r->user);
		list_del(&r->list);
		kfree(r);
		atomic64_inc(&dev->stats.unmaps);
	}
}

static void uml_vduse_inject(struct uml_vduse_dev *dev, unsigned int index)
{
	struct uml_vduse_vq *vq = &dev->vqs[index];

	if (uml_vduse_user_vq_inject_irq(dev->fd, index))
		atomic64_inc(&vq->stats.inject_errors);
	else
		atomic64_inc(&vq->stats.injected);
}

static u16 uml_vduse_used_idx(struct uml_vduse_vq *vq)
{
	return vringh16_to_cpu(&vq->vrh, READ_ONCE(vq->vrh.vring.used->idx));
}

/*
 * Record in the log that a request was taken. The head can be taken again as
 * soon as its last completion is published, which is before that completion
 * clears its flag, so the lock orders the two.
 */
static void uml_vduse_log_take(struct uml_vduse_vq *vq, u16 head)
{
	struct uml_vduse_log_desc *d = &vq->log->desc[head];
	unsigned long flags;

	spin_lock_irqsave(&vq->lock, flags);
	WRITE_ONCE(d->counter, vq->counter++);
	smp_wmb();
	WRITE_ONCE(d->inflight, 1);
	spin_unlock_irqrestore(&vq->lock, flags);
}

static void uml_vduse_free_replay(struct uml_vduse_vq *vq)
{
	kfree(vq->replay);
	vq->replay = NULL;
	vq->nr_replay = 0;
	vq->replay_pos = 0;
}

/* Take the next request that an earlier guest left outstanding. */
static int uml_vduse_take_replay(struct uml_vduse_vq *vq,
				 struct vringh_kiov *riov,
				 struct vringh_kiov *wiov, u16 *head)
{
	int err;

	*head = vq->replay[vq->replay_pos++];
	if (vq->replay_pos == vq->nr_replay)
		uml_vduse_free_replay(vq);

	err = vringh_chain_kern_range(&vq->vrh, *head, riov, wiov,
				      uml_vduse_getrange, GFP_KERNEL);
	if (err)
		return err;

	atomic64_inc(&vq->stats.replayed);
	return 1;
}

/*
 * Get the next available descriptor chain with its buffers translated to
 * pointers. Requests that an earlier guest left outstanding come first. A
 * chain that cannot be translated is returned to the driver as used with no
 * data. Returns 1 for a chain, 0 if there is none, or an error if the ring
 * itself is invalid.
 */
int uml_vduse_getdesc(struct uml_vduse_dev *dev, unsigned int index,
		      struct vringh_kiov *riov, struct vringh_kiov *wiov,
		      u16 *head)
{
	struct uml_vduse_vq *vq = &dev->vqs[index];
	int err;

	for (;;) {
		if (vq->nr_replay) {
			err = uml_vduse_take_replay(vq, riov, wiov, head);
		} else {
			err = vringh_getdesc_kern_range(&vq->vrh, riov, wiov,
							uml_vduse_getrange,
							head, GFP_KERNEL);
			if (err > 0)
				uml_vduse_log_take(vq, *head);
		}
		if (err > 0) {
			atomic64_inc(&vq->stats.requests);
			if (riov)
				atomic64_add(vringh_kiov_length(riov),
					     &vq->stats.out_bytes);
			return 1;
		}
		if (!err)
			return 0;

		if (*head == vq->vrh.vring.num) {
			pr_err_ratelimited("%s: queue %u is broken: %d\n",
					   dev->name, index, err);
			return err;
		}

		pr_warn_ratelimited("%s: dropping a bad request on queue %u: %d\n",
				    dev->name, index, err);
		atomic64_inc(&vq->stats.bad_requests);
		uml_vduse_complete(dev, index, *head, 0);
	}
}

void uml_vduse_complete(struct uml_vduse_dev *dev, unsigned int index,
			u16 head, u32 len)
{
	struct uml_vduse_vq *vq = &dev->vqs[index];
	unsigned long flags;
	int notify;

	/*
	 * The log's used index trails the ring's only between publishing the
	 * completion and clearing its flag, and then the head is last_head.
	 */
	spin_lock_irqsave(&vq->lock, flags);
	WRITE_ONCE(vq->log->last_head, head);
	smp_wmb();
	vringh_complete_kern(&vq->vrh, head, len);
	WRITE_ONCE(vq->log->desc[head].inflight, 0);
	smp_wmb();
	WRITE_ONCE(vq->log->used_idx,
		   vq->vrh.last_used_idx + vq->vrh.completed);
	notify = vringh_need_notify_kern(&vq->vrh);
	spin_unlock_irqrestore(&vq->lock, flags);

	atomic64_add(len, &vq->stats.in_bytes);
	if (notify <= 0)
		atomic64_inc(&vq->stats.suppressed);
	else
		uml_vduse_inject(dev, index);
}

void uml_vduse_notify_disable(struct uml_vduse_dev *dev, unsigned int index)
{
	vringh_notify_disable_kern(&dev->vqs[index].vrh);
}

/*
 * Enable notifications, and return false if more buffers became available
 * in the meantime.
 */
bool uml_vduse_notify_enable(struct uml_vduse_dev *dev, unsigned int index)
{
	return vringh_notify_enable_kern(&dev->vqs[index].vrh);
}

/*
 * Wait for the driver to make more buffers available, with notifications
 * still disabled, for up to the device's poll time or until the CPU is
 * needed. Return whether it did. Each kick that this saves costs the host a
 * notification and the guest a wakeup.
 */
bool uml_vduse_poll(struct uml_vduse_dev *dev, unsigned int index)
{
	struct uml_vduse_vq *vq = &dev->vqs[index];
	struct vringh *vrh = &vq->vrh;
	u64 end;

	if (!dev->poll_ns)
		return false;

	end = ktime_get_ns() + dev->poll_ns;
	do {
		if (vringh16_to_cpu(vrh, READ_ONCE(vrh->vring.avail->idx)) !=
		    vrh->last_avail_idx) {
			atomic64_inc(&vq->stats.polled);
			return true;
		}
		cpu_relax();
	} while (!need_resched() && ktime_get_ns() < end);

	atomic64_inc(&vq->stats.poll_timeouts);
	return false;
}

static size_t uml_vduse_log_queue_size(struct uml_vduse_dev *dev)
{
	return struct_size_t(struct uml_vduse_log_queue, desc, dev->queue_size);
}

static void uml_vduse_clear_log(struct uml_vduse_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++) {
		struct uml_vduse_vq *vq = &dev->vqs[i];

		memset(vq->log, 0, uml_vduse_log_queue_size(dev));
		vq->counter = 0;
		uml_vduse_free_replay(vq);
	}
}

struct uml_vduse_inflight {
	u64 counter;
	u16 head;
};

static int uml_vduse_cmp_inflight(const void *a, const void *b)
{
	const struct uml_vduse_inflight *x = a, *y = b;

	return x->counter < y->counter ? -1 : x->counter > y->counter;
}

/*
 * Take over a queue that an earlier guest left running. The log marks its
 * outstanding requests, and the next entry of the available ring follows
 * them, since every request taken is either completed or in flight. This
 * runs again after a failed start, so it replaces any earlier replay list.
 */
static int uml_vduse_recover_vq(struct uml_vduse_dev *dev,
				struct uml_vduse_vq *vq)
{
	struct uml_vduse_log_queue *log = vq->log;
	unsigned int num = vq->vrh.vring.num;
	struct uml_vduse_inflight *in;
	unsigned int i, n = 0;
	u16 used;

	uml_vduse_free_replay(vq);
	vq->counter = 0;

	used = uml_vduse_used_idx(vq);
	vq->vrh.last_used_idx = used;

	/* The earlier guest stopped between publishing and logging. */
	if (log->used_idx != used) {
		if (log->last_head < num)
			log->desc[log->last_head].inflight = 0;
		log->used_idx = used;
	}

	in = kmalloc_objs(*in, num);
	if (!in)
		return -ENOMEM;

	for (i = 0; i < dev->queue_size; i++) {
		if (!log->desc[i].inflight)
			continue;
		if (i >= num) {
			log->desc[i].inflight = 0;
			continue;
		}
		in[n].counter = log->desc[i].counter;
		in[n].head = i;
		n++;
	}
	sort(in, n, sizeof(*in), uml_vduse_cmp_inflight, NULL);

	if (n) {
		vq->replay = kmalloc_objs(*vq->replay, n);
		if (!vq->replay) {
			kfree(in);
			return -ENOMEM;
		}
		for (i = 0; i < n; i++)
			vq->replay[i] = in[i].head;
		vq->nr_replay = n;
		vq->counter = in[n - 1].counter + 1;
	}
	vq->vrh.last_avail_idx = used + n;
	kfree(in);

	pr_info("%s: queue %u has %u requests to replay\n", dev->name,
		vq->index, n);
	return 0;
}

static int uml_vduse_start_vq(struct uml_vduse_dev *dev,
			      struct uml_vduse_vq *vq)
{
	struct uml_vduse_user_vq_info info;
	void *desc, *avail, *used;
	size_t num;
	int err;

	err = uml_vduse_user_vq_get_info(dev->fd, vq->index, &info);
	if (err)
		return err;
	vq->ready = info.ready;
	if (!vq->ready)
		return 0;

	num = info.num;
	desc = uml_vduse_translate(dev, info.desc_addr,
				   num * sizeof(struct vring_desc), false);
	avail = uml_vduse_translate(dev, info.driver_addr,
				    sizeof(struct vring_avail) +
				    (num + 1) * sizeof(__virtio16), false);
	used = uml_vduse_translate(dev, info.device_addr,
				   sizeof(struct vring_used) +
				   num * sizeof(struct vring_used_elem) +
				   sizeof(__virtio16), true);
	if (!desc || !avail || !used)
		return -EFAULT;

	err = vringh_init_kern(&vq->vrh, dev->features, num, true, desc,
			       avail, used);
	if (err)
		return err;
	vq->vrh.last_avail_idx = info.avail_index;
	vq->vrh.last_used_idx = info.avail_index;

	/* The log's used index follows the ring's from the start. */
	if (dev->reconnect) {
		err = uml_vduse_recover_vq(dev, vq);
		if (err)
			return err;
	} else {
		WRITE_ONCE(vq->log->used_idx, uml_vduse_used_idx(vq));
	}

	return uml_vduse_user_vq_set_kickfd(dev->fd, vq->index, vq->kickfd);
}

static void uml_vduse_stop_vqs(struct uml_vduse_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++) {
		WRITE_ONCE(dev->vqs[i].running, false);
		synchronize_irq(dev->vqs[i].irq);
	}
}

static int uml_vduse_start(struct uml_vduse_dev *dev)
{
	unsigned int i;
	int err;

	err = uml_vduse_user_get_features(dev->fd, &dev->features);
	if (err)
		return err;

	for (i = 0; i < dev->num_queues; i++) {
		err = uml_vduse_start_vq(dev, &dev->vqs[i]);
		if (err)
			return err;
	}

	/*
	 * No request of this start has been translated yet, so none can point
	 * into the host's bounce pages when guest pages replace them.
	 */
	dev->umem_registered = false;
	WRITE_ONCE(dev->umem_wanted, dev->bounce_guest);

	err = dev->ops->start(dev->priv);
	if (err)
		return err;
	dev->running = true;

	/*
	 * Kicks that arrived before the queues were running were dropped. An
	 * earlier guest may have stopped after publishing a completion and
	 * before raising its interrupt, so a reconnecting device raises one on
	 * each queue.
	 */
	for (i = 0; i < dev->num_queues; i++) {
		if (!dev->vqs[i].ready)
			continue;
		WRITE_ONCE(dev->vqs[i].running, true);
		if (dev->reconnect)
			uml_vduse_inject(dev, i);
		dev->ops->kick(dev->priv, i);
	}
	dev->reconnect = false;

	return 0;
}

static void uml_vduse_stop(struct uml_vduse_dev *dev)
{
	uml_vduse_stop_vqs(dev);
	dev->ops->stop(dev->priv);
	dev->running = false;
}

/*
 * The host frees and reallocates the rings and the bounce region without a
 * message, so a reset drops every mapping.
 */
static int uml_vduse_set_status(struct uml_vduse_dev *dev, u8 status)
{
	int err = 0;

	if ((status & VIRTIO_CONFIG_S_DRIVER_OK) && !dev->running) {
		err = uml_vduse_start(dev);
		if (err)
			pr_err("%s: cannot start the device: %d\n", dev->name,
			       err);
		else
			atomic64_inc(&dev->stats.starts);
	} else if (!(status & VIRTIO_CONFIG_S_DRIVER_OK) && dev->running) {
		uml_vduse_stop(dev);
	}

	if (!status) {
		uml_vduse_unmap_range(dev, 0, U64_MAX);
		uml_vduse_clear_log(dev);
		dev->reconnect = false;
		atomic64_inc(&dev->stats.resets);
	}

	return err;
}

/*
 * Start the queues that the driver had running when the earlier guest
 * stopped. With no ready queue, the driver has not started the device, and
 * its messages start it as usual. If the start fails, the device stays
 * reconnecting, so that a later DRIVER_OK takes over the queues again
 * instead of starting them from the beginning.
 */
static void uml_vduse_resume(struct uml_vduse_dev *dev)
{
	struct uml_vduse_user_vq_info info;
	bool ready = false;
	unsigned int i;
	int err;

	dev->resume = false;
	atomic64_inc(&dev->stats.reconnects);

	for (i = 0; i < dev->num_queues; i++) {
		if (!uml_vduse_user_vq_get_info(dev->fd, i, &info) && info.ready)
			ready = true;
	}

	if (!ready) {
		dev->reconnect = false;
		return;
	}

	err = uml_vduse_start(dev);
	if (err)
		pr_err("%s: cannot resume the device: %d\n", dev->name, err);
	else
		atomic64_inc(&dev->stats.starts);
}

static void uml_vduse_msg_work(struct work_struct *work)
{
	struct uml_vduse_dev *dev = container_of(work, struct uml_vduse_dev,
						 msg_work);
	struct uml_vduse_user_msg msg;
	u16 avail_index;
	bool ok;
	int err;

	/*
	 * A reconnecting device takes over its queues before handling the
	 * messages that the earlier guest left unanswered.
	 */
	if (dev->resume)
		uml_vduse_resume(dev);

	while (!(err = uml_vduse_user_read_msg(dev->fd, &msg))) {
		avail_index = 0;
		ok = true;
		atomic64_inc(&dev->stats.msgs[msg.type]);

		switch (msg.type) {
		case UML_VDUSE_MSG_GET_VQ_STATE:
			if (msg.index < dev->num_queues)
				avail_index = dev->vqs[msg.index].vrh.last_avail_idx;
			else
				ok = false;
			break;
		case UML_VDUSE_MSG_SET_STATUS:
			ok = !uml_vduse_set_status(dev, msg.status);
			break;
		case UML_VDUSE_MSG_UPDATE_IOTLB:
			/* The rings stay mapped while the device runs. */
			if (!dev->running)
				uml_vduse_unmap_range(dev, msg.start, msg.last);
			break;
		default:
			ok = false;
			break;
		}

		err = uml_vduse_user_reply(dev->fd, &msg, ok, avail_index);
		if (err)
			pr_warn("%s: cannot answer a message: %d\n", dev->name,
				err);
	}

	if (err != -EAGAIN)
		pr_warn("%s: cannot read a message: %d\n", dev->name, err);
}

static irqreturn_t uml_vduse_msg_interrupt(int irq, void *opaque)
{
	struct uml_vduse_dev *dev = opaque;

	schedule_work(&dev->msg_work);
	return IRQ_HANDLED;
}

static irqreturn_t uml_vduse_kick_interrupt(int irq, void *opaque)
{
	struct uml_vduse_vq *vq = opaque;
	struct uml_vduse_dev *dev = vq->dev;
	u64 v;
	int r;

	/* A read clears the whole count. */
	do {
		r = os_read_file(vq->kickfd, &v, sizeof(v));
	} while (r == -EINTR);

	atomic64_inc(&vq->stats.kick_irqs);
	if (r != sizeof(v))
		return IRQ_HANDLED;

	atomic64_add(v, &vq->stats.kicks);
	if (READ_ONCE(vq->running))
		dev->ops->kick(dev->priv, vq->index);
	else
		atomic64_add(v, &vq->stats.dropped_kicks);

	return IRQ_HANDLED;
}

static int __init uml_vduse_request_irq(int fd, irq_handler_t handler,
					void *opaque)
{
	int irq, err;

	irq = um_request_irq(UM_IRQ_ALLOC, fd, IRQ_READ, handler, 0,
			     "vduse-uml", opaque);
	if (irq < 0)
		return irq;

	err = add_sigio_fd(fd);
	if (err) {
		um_free_irq(irq, opaque);
		return err;
	}

	return irq;
}

static void uml_vduse_free_irq(int fd, int irq, void *opaque)
{
	ignore_sigio_fd(fd);
	um_free_irq(irq, opaque);
}

static u64 __init uml_vduse_features(const struct uml_vduse_config *cfg)
{
	return cfg->features | BIT_ULL(VIRTIO_F_VERSION_1) |
	       BIT_ULL(VIRTIO_F_ACCESS_PLATFORM) |
	       BIT_ULL(VIRTIO_RING_F_INDIRECT_DESC) |
	       BIT_ULL(VIRTIO_RING_F_EVENT_IDX);
}

/*
 * Create the host device. A device with the same name that no process opens
 * and no driver uses is destroyed and created again. Returns 1 for one that
 * is still in use, to reconnect to.
 */
static int __init uml_vduse_create_host(struct uml_vduse_dev *dev,
					const struct uml_vduse_config *cfg)
{
	u64 features = uml_vduse_features(cfg);
	int err;

	if (uml_vduse_control < 0) {
		uml_vduse_control = uml_vduse_user_open_control();
		if (uml_vduse_control < 0)
			return uml_vduse_control;
	}

	err = uml_vduse_user_create(uml_vduse_control, dev->name,
				    cfg->device_id, features, cfg->num_queues,
				    PAGE_SIZE, cfg->config, cfg->config_size);
	if (err != -EEXIST)
		return err;

	err = uml_vduse_user_destroy(uml_vduse_control, dev->name);
	if (err == -EBUSY)
		return 1;
	if (err)
		return err;

	return uml_vduse_user_create(uml_vduse_control, dev->name,
				     cfg->device_id, features,
				     cfg->num_queues, PAGE_SIZE, cfg->config,
				     cfg->config_size);
}

/*
 * Check that a device left by an earlier guest has this device's queues, and
 * that its driver uses no feature this device does not offer. VDUSE cannot
 * report the device type or configuration space, so those are assumed to be
 * the same.
 */
static int __init uml_vduse_check_reconnect(struct uml_vduse_dev *dev,
					    u64 offered)
{
	struct uml_vduse_user_vq_info info;
	unsigned int i;
	u64 features;
	int err;

	err = uml_vduse_user_get_features(dev->fd, &features);
	if (err == -EPERM) {
		pr_err("%s: the device stopped working after a message timed out, run 'vdpa dev del %s' on the host\n",
		       dev->name, dev->name);
		return err;
	}
	if (err)
		return err;

	if (features & ~offered) {
		pr_err("%s: the driver uses features 0x%llx that this device does not offer\n",
		       dev->name, features & ~offered);
		return -EINVAL;
	}

	for (i = 0; i < dev->num_queues; i++) {
		err = uml_vduse_user_vq_get_info(dev->fd, i, &info);
		if (err || info.num > dev->queue_size)
			goto mismatch;
	}
	if (!uml_vduse_user_vq_get_info(dev->fd, dev->num_queues, &info))
		goto mismatch;
	return 0;

mismatch:
	pr_err("%s: the device has different queues from this one\n",
	       dev->name);
	return -EINVAL;
}

static bool __init uml_vduse_log_matches(struct uml_vduse_dev *dev,
					 u64 node_dev, u64 node_ino)
{
	struct uml_vduse_log_header *hdr = dev->log;

	return hdr->magic == UML_VDUSE_LOG_MAGIC &&
	       hdr->version == UML_VDUSE_LOG_VERSION &&
	       hdr->num_queues == dev->num_queues &&
	       hdr->queue_size == dev->queue_size &&
	       hdr->node_dev == node_dev && hdr->node_ino == node_ino;
}

/*
 * Map the in-flight log. A new device starts with an empty log. Reconnecting
 * needs the log of the earlier guest, since without it the requests that
 * guest left outstanding cannot be found.
 */
static int __init uml_vduse_open_log(struct uml_vduse_dev *dev)
{
	size_t qsize = uml_vduse_log_queue_size(dev);
	struct uml_vduse_log_header *hdr;
	u64 node_dev, node_ino;
	unsigned int i;
	int err;

	err = uml_vduse_user_node_id(dev->fd, &node_dev, &node_ino);
	if (err)
		return err;

	if (dev->log_path) {
		dev->log_fd = uml_vduse_user_open_log(dev->log_path);
		if (dev->log_fd < 0)
			return dev->log_fd;
	}

	dev->log_size = sizeof(*hdr) + dev->num_queues * qsize;
	err = uml_vduse_user_map_log(dev->log_fd, dev->log_size,
				     !dev->reconnect, &dev->log);
	if (err == -EINVAL ||
	    (!err && dev->reconnect &&
	     !uml_vduse_log_matches(dev, node_dev, node_ino))) {
		pr_err("%s: the in-flight log does not describe this device\n",
		       dev->name);
		return -EINVAL;
	}
	if (err)
		return err;

	hdr = dev->log;
	if (!dev->reconnect) {
		memset(dev->log, 0, dev->log_size);
		hdr->magic = UML_VDUSE_LOG_MAGIC;
		hdr->version = UML_VDUSE_LOG_VERSION;
		hdr->num_queues = dev->num_queues;
		hdr->queue_size = dev->queue_size;
		hdr->node_dev = node_dev;
		hdr->node_ino = node_ino;
	}

	for (i = 0; i < dev->num_queues; i++)
		dev->vqs[i].log = dev->log + sizeof(*hdr) + i * qsize;
	return 0;
}

static void __init uml_vduse_close_log(struct uml_vduse_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++)
		dev->vqs[i].log = NULL;
	if (dev->log)
		uml_vduse_user_unmap_log(dev->log, dev->log_size);
	dev->log = NULL;
	if (dev->log_path && dev->log_fd >= 0)
		os_close_file(dev->log_fd);
}

static void uml_vduse_close_vqs(struct uml_vduse_dev *dev,
				       unsigned int n)
{
	while (n--) {
		uml_vduse_free_irq(dev->vqs[n].kickfd, dev->vqs[n].irq,
				   &dev->vqs[n]);
		os_close_file(dev->vqs[n].kickfd);
	}
}

/* A reconnecting guest keeps the sizes that the device already has. */
static int __init uml_vduse_open_vqs(struct uml_vduse_dev *dev)
{
	unsigned int i;
	int err;

	for (i = 0; i < dev->num_queues; i++) {
		struct uml_vduse_vq *vq = &dev->vqs[i];

		if (!dev->reconnect) {
			err = uml_vduse_user_vq_setup(dev->fd, i,
						      dev->queue_size);
			if (err)
				goto close;
		}

		vq->kickfd = uml_vduse_user_create_kickfd();
		if (vq->kickfd < 0) {
			err = vq->kickfd;
			goto close;
		}

		vq->irq = uml_vduse_request_irq(vq->kickfd,
						uml_vduse_kick_interrupt, vq);
		if (vq->irq < 0) {
			err = vq->irq;
			os_close_file(vq->kickfd);
			goto close;
		}

		/*
		 * Each queue's kicks, and the backend's work that they
		 * schedule, run on a CPU of its own where there are enough.
		 */
		irq_set_affinity(vq->irq,
				 cpumask_of(cpumask_local_spread(i, NUMA_NO_NODE)));
	}

	return 0;

close:
	uml_vduse_close_vqs(dev, i);
	return err;
}

static const struct uml_vduse_backend_ops *__init
uml_vduse_find_backend(const char *type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(uml_vduse_backends); i++) {
		if (!strcmp(uml_vduse_backends[i]->type, type))
			return uml_vduse_backends[i];
	}
	return NULL;
}

static int __init uml_vduse_setup(struct uml_vduse_dev *dev)
{
	struct uml_vduse_config cfg = {};
	unsigned int i;
	int err;

	dev->ops = uml_vduse_find_backend(dev->type);
	if (!dev->ops)
		return -ENODEV;

	dev->priv = dev->ops->setup(dev, dev->opts, &cfg);
	if (IS_ERR(dev->priv))
		return PTR_ERR(dev->priv);

	if (!cfg.num_queues || !is_power_of_2(cfg.queue_size))
		return -EINVAL;

	dev->num_queues = cfg.num_queues;
	dev->queue_size = cfg.queue_size;
	dev->vqs = kzalloc_objs(*dev->vqs, dev->num_queues);
	if (!dev->vqs)
		return -ENOMEM;

	for (i = 0; i < dev->num_queues; i++) {
		dev->vqs[i].dev = dev;
		dev->vqs[i].index = i;
		spin_lock_init(&dev->vqs[i].lock);
	}

	err = uml_vduse_create_host(dev, &cfg);
	if (err < 0)
		return err;
	dev->reconnect = dev->resume = err;

	dev->fd = uml_vduse_user_open(dev->name);
	if (dev->fd < 0) {
		err = dev->fd;
		if (err == -EBUSY)
			pr_err("%s: another process serves the device\n",
			       dev->name);
		goto destroy;
	}

	if (dev->reconnect) {
		err = uml_vduse_check_reconnect(dev, uml_vduse_features(&cfg));
		if (err)
			goto close;
	}

	err = uml_vduse_open_log(dev);
	if (err)
		goto close;

	err = uml_vduse_open_vqs(dev);
	if (err)
		goto close;

	dev->irq = uml_vduse_request_irq(dev->fd, uml_vduse_msg_interrupt, dev);
	if (dev->irq < 0) {
		err = dev->irq;
		uml_vduse_close_vqs(dev, dev->num_queues);
		goto close;
	}

	pr_info("%s: %s %s device\n", dev->name,
		dev->reconnect ? "reconnecting to the" : "created", dev->type);

	/*
	 * Messages sent before the IRQ existed raise no event. A reconnecting
	 * device resumes in the work too.
	 */
	schedule_work(&dev->msg_work);
	return 0;

close:
	uml_vduse_close_log(dev);
	os_close_file(dev->fd);
destroy:
	if (!dev->reconnect)
		uml_vduse_user_destroy(uml_vduse_control, dev->name);
	return err;
}

/* Take the transport's own options out of the backend's. */
static int __init uml_vduse_parse_opts(struct uml_vduse_dev *dev, char *opts)
{
	size_t len = 0;
	char *opt;

	dev->log_fd = -1;
	dev->opts = kzalloc(opts ? strlen(opts) + 1 : 1, GFP_KERNEL);
	if (!dev->opts)
		return -ENOMEM;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "log=")) {
			dev->log_path = opt + strlen("log=");
		} else if (strstarts(opt, "log_fd=")) {
			if (kstrtoint(opt + strlen("log_fd="), 10, &dev->log_fd) ||
			    dev->log_fd < 0)
				return -EINVAL;
		} else if (!strcmp(opt, "bounce=guest")) {
			dev->bounce_guest = true;
		} else if (!strcmp(opt, "bounce=host")) {
			dev->bounce_guest = false;
		} else if (strstarts(opt, "poll=")) {
			unsigned int usecs;

			if (kstrtouint(opt + strlen("poll="), 10, &usecs))
				return -EINVAL;
			dev->poll_ns = (u64)usecs * NSEC_PER_USEC;
		} else if (*opt) {
			if (len)
				dev->opts[len++] = ',';
			strcpy(dev->opts + len, opt);
			len += strlen(opt);
		}
	}

	if (dev->log_path && dev->log_fd >= 0)
		return -EINVAL;
	if (!dev->log_path && dev->log_fd < 0) {
		pr_err("%s: the device needs an in-flight log, given with log=<host path> or log_fd=<n>\n",
		       dev->name);
		return -EINVAL;
	}
	return 0;
}

/* Add a device given as <type>,<name>[,<options>]. */
static void __init uml_vduse_add(const char *arg)
{
	struct uml_vduse_dev *dev;
	char *sep;
	int err;

	dev = kzalloc_obj(*dev);
	if (!dev)
		return;

	INIT_LIST_HEAD(&dev->regions);
	mutex_init(&dev->map_lock);
	mutex_init(&dev->umem_lock);
	INIT_WORK(&dev->msg_work, uml_vduse_msg_work);

	dev->type = kstrdup(arg, GFP_KERNEL);
	if (!dev->type)
		goto free;

	sep = strchr(dev->type, ',');
	if (!sep || !sep[1]) {
		pr_err("invalid device '%s'\n", arg);
		goto free;
	}
	*sep = '\0';
	dev->name = sep + 1;

	sep = strchr(dev->name, ',');
	if (sep)
		*sep = '\0';

	err = uml_vduse_parse_opts(dev, sep ? sep + 1 : NULL);
	if (err) {
		pr_err("%s: invalid options\n", dev->name);
		goto free;
	}

	err = uml_vduse_setup(dev);
	if (err) {
		pr_err("%s: cannot create the %s device: %d\n", dev->name,
		       dev->type, err);
		if (dev->ops && !IS_ERR_OR_NULL(dev->priv))
			dev->ops->release(dev->priv);
		goto free;
	}

	list_add_tail(&dev->list, &uml_vduse_devices);
	return;

free:
	kfree(dev->opts);
	kfree(dev->vqs);
	kfree(dev->type);
	kfree(dev);
}

static int uml_vduse_cmdline_set(const char *val,
				 const struct kernel_param *kp)
{
	if (uml_vduse_nr_args == UML_VDUSE_MAX_DEVICES)
		return -ENOSPC;
	uml_vduse_args[uml_vduse_nr_args++] = val;
	return 0;
}

static int uml_vduse_cmdline_get(char *buffer, const struct kernel_param *kp)
{
	return 0;
}

static const struct kernel_param_ops uml_vduse_cmdline_param_ops = {
	.set = uml_vduse_cmdline_set,
	.get = uml_vduse_cmdline_get,
};

device_param_cb(device, &uml_vduse_cmdline_param_ops, NULL, 0400);
__uml_help(uml_vduse_cmdline_param_ops,
"vduse_uml.device=<type>,<name>[,<options>]\n"
"    Export a VirtIO device of the given type to the host through VDUSE as\n"
"    /dev/vduse/<name>. The host attaches it with 'vdpa dev add name <name>\n"
"    mgmtdev vduse'. This parameter can be specified multiple times. The\n"
"    blk type serves a guest block device and takes disk=<disk>, where\n"
"    <disk> is /dev/<name> or <major>:<minor>, and queues=<n>, one queue\n"
"    per guest CPU by default. Each queue's kicks are handled on a CPU of\n"
"    its own where there are enough.\n"
"    Every device needs an in-flight log, a host file that outlives UML,\n"
"    given with log=<host path> or as the open descriptor log_fd=<n>. If\n"
"    the device already exists and is still in use on the host, UML\n"
"    reconnects to it and takes over the outstanding requests that the\n"
"    earlier UML recorded in the log.\n"
"    With bounce=guest, guest pages back VDUSE's bounce region, through\n"
"    which the host's buffers pass, so that backends need not copy them.\n"
"    It uses as much guest memory as the region, 64 MB by default, and\n"
"    pins that memory on the host, where it counts against\n"
"    RLIMIT_MEMLOCK. The default, bounce=host, leaves the region in host\n"
"    memory.\n"
"    With poll=<usecs>, a queue that runs out of buffers keeps looking for\n"
"    more for up to that long before it asks the host for a kick, which\n"
"    saves kicks under load at the cost of guest CPU time. The default is\n"
"    0.\n\n"
);

static const char *const uml_vduse_msg_names[] = {
	[UML_VDUSE_MSG_GET_VQ_STATE] = "msg_get_vq_state",
	[UML_VDUSE_MSG_SET_STATUS] = "msg_set_status",
	[UML_VDUSE_MSG_UPDATE_IOTLB] = "msg_update_iotlb",
	[UML_VDUSE_MSG_UNKNOWN] = "msg_unknown",
};

static void uml_vduse_show_counter(struct seq_file *m, int indent,
				   const char *name, atomic64_t *v)
{
	seq_printf(m, "%*s%-*s %lld\n", indent, "", 20 - indent, name,
		   atomic64_read(v));
}

static void uml_vduse_show_device(struct seq_file *m,
				  struct uml_vduse_dev *dev)
{
	struct uml_vduse_dev_stats *stats = &dev->stats;
	unsigned int i;

	seq_printf(m, "device %s\n", dev->name);
	seq_printf(m, "  %-18s %s\n", "type", dev->type);
	seq_printf(m, "  %-18s %s\n", "state",
		   READ_ONCE(dev->running) ? "running" : "stopped");
	seq_printf(m, "  %-18s %s\n", "bounce_region",
		   READ_ONCE(dev->umem_registered) ? "guest" : "host");
	seq_printf(m, "  %-18s 0x%llx\n", "features", READ_ONCE(dev->features));

	for (i = 0; i < ARRAY_SIZE(uml_vduse_msg_names); i++)
		uml_vduse_show_counter(m, 2, uml_vduse_msg_names[i],
				       &stats->msgs[i]);
	uml_vduse_show_counter(m, 2, "starts", &stats->starts);
	uml_vduse_show_counter(m, 2, "resets", &stats->resets);
	uml_vduse_show_counter(m, 2, "iova_maps", &stats->maps);
	uml_vduse_show_counter(m, 2, "iova_unmaps", &stats->unmaps);
	uml_vduse_show_counter(m, 2, "iova_map_errors", &stats->map_errors);
	uml_vduse_show_counter(m, 2, "reconnects", &stats->reconnects);
	uml_vduse_show_counter(m, 2, "umem_regs", &stats->umem_regs);

	for (i = 0; i < dev->num_queues; i++) {
		struct uml_vduse_vq_stats *q = &dev->vqs[i].stats;

		seq_printf(m, "  queue %u\n", i);
		uml_vduse_show_counter(m, 4, "kicks", &q->kicks);
		uml_vduse_show_counter(m, 4, "kick_irqs", &q->kick_irqs);
		uml_vduse_show_counter(m, 4, "dropped_kicks",
				       &q->dropped_kicks);
		uml_vduse_show_counter(m, 4, "requests", &q->requests);
		uml_vduse_show_counter(m, 4, "bad_requests",
				       &q->bad_requests);
		uml_vduse_show_counter(m, 4, "out_bytes", &q->out_bytes);
		uml_vduse_show_counter(m, 4, "in_bytes", &q->in_bytes);
		uml_vduse_show_counter(m, 4, "injected", &q->injected);
		uml_vduse_show_counter(m, 4, "suppressed", &q->suppressed);
		uml_vduse_show_counter(m, 4, "inject_errors",
				       &q->inject_errors);
		uml_vduse_show_counter(m, 4, "replayed", &q->replayed);
		uml_vduse_show_counter(m, 4, "polled", &q->polled);
		uml_vduse_show_counter(m, 4, "poll_timeouts",
				       &q->poll_timeouts);
	}
}

static int __maybe_unused uml_vduse_proc_show(struct seq_file *m, void *v)
{
	struct uml_vduse_dev *dev;

	list_for_each_entry(dev, &uml_vduse_devices, list)
		uml_vduse_show_device(m, dev);

	return 0;
}

/*
 * Stop the backend, which completes the requests it has taken, close the
 * device's fds and destroy it on the host. A device that is still on the
 * host's vDPA bus cannot be destroyed, and the next guest reconnects to it.
 */
static void uml_vduse_release(struct uml_vduse_dev *dev)
{
	int err;

	/* Once the IRQ is gone, no message can start the backend again. */
	uml_vduse_free_irq(dev->fd, dev->irq, dev);
	cancel_work_sync(&dev->msg_work);

	if (dev->running)
		uml_vduse_stop(dev);
	uml_vduse_unmap_range(dev, 0, U64_MAX);
	uml_vduse_close_vqs(dev, dev->num_queues);
	os_close_file(dev->fd);
	/* Closing the fd made VDUSE drop its pins on these pages. */
	uml_vduse_free_umem(dev);

	/* A device still on the bus keeps its log for the next guest. */
	err = uml_vduse_user_destroy(uml_vduse_control, dev->name);
	if (err == -EBUSY) {
		pr_info("%s: the device stays on the host's vDPA bus for the next guest\n",
			dev->name);
	} else if (err) {
		pr_warn("%s: cannot destroy the device: %d\n", dev->name, err);
	} else {
		pr_info("%s: destroyed the device\n", dev->name);
		if (dev->log_path)
			uml_vduse_user_remove_log(dev->log_path);
		else
			uml_vduse_user_truncate_log(dev->log_fd);
	}
}

/* This runs before device_shutdown(), while the backends' disks still work. */
static int uml_vduse_reboot(struct notifier_block *nb, unsigned long code,
			    void *unused)
{
	struct uml_vduse_dev *dev;

	list_for_each_entry(dev, &uml_vduse_devices, list)
		uml_vduse_release(dev);

	return NOTIFY_DONE;
}

static struct notifier_block uml_vduse_reboot_notifier = {
	.notifier_call = uml_vduse_reboot,
};

static int __init uml_vduse_init(void)
{
	int i;

	if (!uml_vduse_nr_args)
		return 0;

	sigio_broken();

	for (i = 0; i < uml_vduse_nr_args; i++)
		uml_vduse_add(uml_vduse_args[i]);

	proc_create_single("vduse_uml", 0444, NULL, uml_vduse_proc_show);
	register_reboot_notifier(&uml_vduse_reboot_notifier);
	return 0;
}
late_initcall(uml_vduse_init);
