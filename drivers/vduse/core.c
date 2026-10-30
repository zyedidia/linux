// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-guest: " fmt

#include <linux/cpumask.h>
#include <linux/module.h>
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
#include <linux/vduse_guest.h>
#include <linux/workqueue.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ring.h>

#define VDUSE_GUEST_MAX_DEVICES 8
#define VDUSE_GUEST_LOG_MAGIC 0x676f6c76
#define VDUSE_GUEST_LOG_VERSION 2

/*
 * The in-flight log lives in host storage that outlives the guest, so that
 * the next guest can tell which requests were outstanding. A header is
 * followed by one area per queue, with an entry for each descriptor. Each
 * area starts where the previous one ends. The header names the host device,
 * which is new each time the device is created.
 */
struct vduse_guest_log_header {
	u32 magic;
	u32 version;
	u32 num_queues;
	u32 pad;
	u64 node_dev;
	u64 node_ino;
};

struct vduse_guest_log_desc {
	u8 inflight;
	u8 pad[7];
	/* The order in which the requests were taken. */
	u64 counter;
};

struct vduse_guest_log_queue {
	/* The used index and the head of the last completion. */
	u16 used_idx;
	u16 last_head;
	/* The number of entries, which is the queue's size. */
	u16 size;
	u8 pad[10];
	struct vduse_guest_log_desc desc[];
};

struct vduse_guest_mapping {
	struct vduse_guest_region region;
	struct list_head list;
};

/*
 * Counters of a queue. Kicks is the sum of the kicks that the host reported,
 * so it counts the host driver's notifications, and kick_irqs counts the
 * wakeups that reported them. Requests and out_bytes count the chains taken
 * and the sizes of their readable buffers. In_bytes is the sum of the used
 * lengths. Each completion either injects an interrupt or is suppressed
 * because the driver does not want one.
 */
struct vduse_guest_vq_stats {
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

struct vduse_guest_dev_stats {
	atomic64_t msgs[VDUSE_GUEST_MSG_UNKNOWN + 1];
	atomic64_t starts;
	atomic64_t start_errors;
	atomic64_t resets;
	atomic64_t maps;
	atomic64_t unmaps;
	atomic64_t map_errors;
	atomic64_t reconnects;
	atomic64_t umem_regs;
};

struct vduse_guest_vq {
	struct vduse_guest_dev *dev;
	unsigned int index;
	/* The largest size that the driver may give the queue. */
	u16 size;
	struct vringh vrh;
	/*
	 * Protects the used ring and the log. The available ring is only
	 * accessed from the backend's processing of the queue.
	 */
	spinlock_t lock;
	bool ready;
	bool running;

	/* The queue's area of the in-flight log. */
	struct vduse_guest_log_queue *log;
	u64 counter;
	/* Requests of an earlier guest to take again, oldest first. */
	u16 *replay;
	unsigned int nr_replay;
	unsigned int replay_pos;

	struct vduse_guest_vq_stats stats;
};

struct vduse_guest_dev {
	char *type;
	char *name;
	/* The options left for the backend. */
	char *opts;

	const struct vduse_guest_backend_ops *ops;
	void *priv;

	const struct vduse_guest_host_ops *hops;
	void *host_data;
	struct work_struct msg_work;

	unsigned int num_queues;
	struct vduse_guest_vq *vqs;

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

	/* The in-flight log. */
	void *log;
	size_t log_size;

	/* Regions of the IOVA space mapped into the guest, under map_lock. */
	struct list_head regions;
	struct mutex map_lock;

	/*
	 * Guest pages that back the bounce region, so that backends get guest
	 * pages for the host's buffers. The host hands chunks of contiguous
	 * pages to VDUSE. Umem_lock serializes the registration, and
	 * umem_wanted asks for one at the next translation. Bounce_guest is set
	 * by bounce=guest, without which the region stays in host memory.
	 */
	bool bounce_guest;
	struct mutex umem_lock;
	struct page **umem_chunks;
	unsigned int umem_nr_chunks;
	unsigned int umem_shift;
	u64 umem_size;
	bool umem_wanted;
	bool umem_registered;

	struct vduse_guest_dev_stats stats;
	struct list_head list;
};

static const struct vduse_guest_backend_ops *const vduse_guest_backends[] = {
#ifdef CONFIG_VDUSE_GUEST_BLK
	&vduse_guest_blk_ops,
#endif
#ifdef CONFIG_VDUSE_GUEST_WLAN
	&vduse_guest_wlan_ops,
#endif
};

static const char *vduse_guest_args[VDUSE_GUEST_MAX_DEVICES];
static int vduse_guest_nr_args;

static LIST_HEAD(vduse_guest_hosts);
static DEFINE_MUTEX(vduse_guest_hosts_lock);

/* The devices that were created, which is fixed after boot. */
static LIST_HEAD(vduse_guest_devices);

const char *vduse_guest_name(struct vduse_guest_dev *dev)
{
	return dev->name;
}

unsigned int vduse_guest_num_queues(struct vduse_guest_dev *dev)
{
	return dev->num_queues;
}

void *vduse_guest_host_data(struct vduse_guest_dev *dev)
{
	return dev->host_data;
}

void vduse_guest_set_host_data(struct vduse_guest_dev *dev, void *data)
{
	dev->host_data = data;
}

void vduse_guest_register_host(struct vduse_guest_host *host)
{
	guard(mutex)(&vduse_guest_hosts_lock);
	list_add_tail(&host->list, &vduse_guest_hosts);
}

/* A host that serves the name comes before one that serves every name. */
static struct vduse_guest_host *vduse_guest_find_host(const char *name)
{
	struct vduse_guest_host *host, *any = NULL;

	guard(mutex)(&vduse_guest_hosts_lock);

	list_for_each_entry(host, &vduse_guest_hosts, list) {
		if (!host->name && !any)
			any = host;
		else if (host->name && !strcmp(host->name, name))
			return host;
	}
	return any;
}

/*
 * Find the region that holds an IOVA, and map it on a miss. Regions are only
 * unmapped while the queues are stopped, so a region stays valid after the
 * lock is dropped.
 */
static struct vduse_guest_mapping *
vduse_guest_find_region(struct vduse_guest_dev *dev, u64 iova)
{
	struct vduse_guest_mapping *m;
	int err;

	guard(mutex)(&dev->map_lock);

	list_for_each_entry(m, &dev->regions, list) {
		if (iova >= m->region.start && iova <= m->region.last)
			return m;
	}

	m = kzalloc_obj(*m);
	if (!m)
		return NULL;

	err = dev->hops->map(dev, iova, &m->region);
	if (err) {
		atomic64_inc(&dev->stats.map_errors);
		pr_warn_ratelimited("%s: cannot map IOVA 0x%llx: %d\n",
				    dev->name, iova, err);
		kfree(m);
		return NULL;
	}
	list_add(&m->list, &dev->regions);
	atomic64_inc(&dev->stats.maps);
	return m;
}

static void *vduse_guest_translate(struct vduse_guest_dev *dev, u64 iova,
				   size_t len, bool write)
{
	struct vduse_guest_mapping *m;
	u64 last;

	if (!len || check_add_overflow(iova, (u64)len - 1, &last))
		return NULL;

	m = vduse_guest_find_region(dev, iova);
	if (!m || last > m->region.last)
		return NULL;
	if (write ? !m->region.writable : !m->region.readable)
		return NULL;
	return m->region.addr + (iova - m->region.start);
}

static void vduse_guest_free_umem(struct vduse_guest_dev *dev)
{
	unsigned int i, order = dev->umem_shift - PAGE_SHIFT;

	dev->hops->unmap_umem(dev);
	for (i = 0; i < dev->umem_nr_chunks; i++) {
		if (dev->umem_chunks[i])
			__free_pages(dev->umem_chunks[i], order);
	}
	kfree(dev->umem_chunks);
	dev->umem_chunks = NULL;
	dev->umem_nr_chunks = 0;
	dev->umem_size = 0;
}

/*
 * Allocate guest pages for a bounce region of the given size, in chunks of
 * the largest order the page allocator gives, and have the host prepare
 * them. The chunks are compound pages, so that a reference taken on any of
 * their pages is counted.
 */
static int vduse_guest_alloc_umem(struct vduse_guest_dev *dev, u64 size)
{
	unsigned int i, nr, order = min(get_order(size), MAX_PAGE_ORDER);
	int err;

	if (!PAGE_ALIGNED(size))
		return -EINVAL;

	vduse_guest_free_umem(dev);
	dev->umem_shift = PAGE_SHIFT + order;
	nr = DIV_ROUND_UP(size, PAGE_SIZE << order);
	dev->umem_chunks = kzalloc_objs(*dev->umem_chunks, nr);
	if (!dev->umem_chunks)
		return -ENOMEM;
	dev->umem_nr_chunks = nr;

	for (i = 0; i < nr; i++) {
		dev->umem_chunks[i] = alloc_pages(GFP_KERNEL | __GFP_NOWARN |
						  __GFP_COMP, order);
		if (!dev->umem_chunks[i]) {
			vduse_guest_free_umem(dev);
			return -ENOMEM;
		}
	}

	err = dev->hops->map_umem(dev, dev->umem_chunks, nr, dev->umem_shift,
				  size);
	if (err) {
		vduse_guest_free_umem(dev);
		return err;
	}
	dev->umem_size = size;
	return 0;
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
static void vduse_guest_register_umem(struct vduse_guest_dev *dev)
{
	u64 size;
	int err;

	guard(mutex)(&dev->umem_lock);
	if (!dev->umem_wanted)
		return;

	err = dev->hops->bounce_size(dev, &size);
	if (err == -EINVAL)
		return;

	if (!err && size != dev->umem_size)
		err = vduse_guest_alloc_umem(dev, size);
	if (!err) {
		dev->hops->dereg_umem(dev, size);
		err = dev->hops->reg_umem(dev, size);
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
static bool vduse_guest_getrange(struct vringh *vrh, u64 addr,
				 struct vringh_range *range)
{
	struct vduse_guest_vq *vq = container_of(vrh, struct vduse_guest_vq,
						 vrh);
	struct vduse_guest_dev *dev = vq->dev;
	struct vduse_guest_mapping *m;

	if (smp_load_acquire(&dev->umem_wanted))
		vduse_guest_register_umem(dev);

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

	m = vduse_guest_find_region(dev, addr);
	if (!m)
		return false;

	range->start = m->region.start;
	range->end_incl = m->region.last;
	range->offset = (unsigned long)m->region.addr - m->region.start;
	return true;
}

static void vduse_guest_unmap_range(struct vduse_guest_dev *dev, u64 start,
				    u64 last)
{
	struct vduse_guest_mapping *m, *n;

	guard(mutex)(&dev->map_lock);

	list_for_each_entry_safe(m, n, &dev->regions, list) {
		if (m->region.last < start || m->region.start > last)
			continue;
		dev->hops->unmap(dev, &m->region);
		list_del(&m->list);
		kfree(m);
		atomic64_inc(&dev->stats.unmaps);
	}
}

static void vduse_guest_inject(struct vduse_guest_dev *dev,
			       unsigned int index)
{
	struct vduse_guest_vq *vq = &dev->vqs[index];

	if (dev->hops->inject(dev, index))
		atomic64_inc(&vq->stats.inject_errors);
	else
		atomic64_inc(&vq->stats.injected);
}

static u16 vduse_guest_used_idx(struct vduse_guest_vq *vq)
{
	return vringh16_to_cpu(&vq->vrh, READ_ONCE(vq->vrh.vring.used->idx));
}

/*
 * Record in the log that a request was taken. The head can be taken again as
 * soon as its last completion is published, which is before that completion
 * clears its flag, so the lock orders the two.
 */
static void vduse_guest_log_take(struct vduse_guest_vq *vq, u16 head)
{
	struct vduse_guest_log_desc *d = &vq->log->desc[head];
	unsigned long flags;

	spin_lock_irqsave(&vq->lock, flags);
	WRITE_ONCE(d->counter, vq->counter++);
	smp_wmb();
	WRITE_ONCE(d->inflight, 1);
	spin_unlock_irqrestore(&vq->lock, flags);
}

static void vduse_guest_free_replay(struct vduse_guest_vq *vq)
{
	kfree(vq->replay);
	vq->replay = NULL;
	vq->nr_replay = 0;
	vq->replay_pos = 0;
}

/* Take the next request that an earlier guest left outstanding. */
static int vduse_guest_take_replay(struct vduse_guest_vq *vq,
				   struct vringh_kiov *riov,
				   struct vringh_kiov *wiov, u16 *head)
{
	int err;

	*head = vq->replay[vq->replay_pos++];
	if (vq->replay_pos == vq->nr_replay)
		vduse_guest_free_replay(vq);

	err = vringh_chain_kern_range(&vq->vrh, *head, riov, wiov,
				      vduse_guest_getrange, GFP_KERNEL);
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
int vduse_guest_getdesc(struct vduse_guest_dev *dev, unsigned int index,
			struct vringh_kiov *riov, struct vringh_kiov *wiov,
			u16 *head)
{
	struct vduse_guest_vq *vq = &dev->vqs[index];
	int err;

	for (;;) {
		if (vq->nr_replay) {
			err = vduse_guest_take_replay(vq, riov, wiov, head);
		} else {
			err = vringh_getdesc_kern_range(&vq->vrh, riov, wiov,
							vduse_guest_getrange,
							head, GFP_KERNEL);
			if (err > 0)
				vduse_guest_log_take(vq, *head);
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
		vduse_guest_complete(dev, index, *head, 0);
	}
}

void vduse_guest_complete(struct vduse_guest_dev *dev, unsigned int index,
			  u16 head, u32 len)
{
	struct vduse_guest_vq *vq = &dev->vqs[index];
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
		vduse_guest_inject(dev, index);
}

void vduse_guest_notify_disable(struct vduse_guest_dev *dev,
				unsigned int index)
{
	vringh_notify_disable_kern(&dev->vqs[index].vrh);
}

/*
 * Enable notifications, and return false if more buffers became available
 * in the meantime.
 */
bool vduse_guest_notify_enable(struct vduse_guest_dev *dev,
			       unsigned int index)
{
	return vringh_notify_enable_kern(&dev->vqs[index].vrh);
}

/*
 * Wait for the driver to make more buffers available, with notifications
 * still disabled, for up to the device's poll time or until the CPU is
 * needed. Return whether it did. Each kick that this saves costs the host a
 * notification and the guest a wakeup.
 */
bool vduse_guest_poll(struct vduse_guest_dev *dev, unsigned int index)
{
	struct vduse_guest_vq *vq = &dev->vqs[index];
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

static size_t vduse_guest_log_queue_size(struct vduse_guest_vq *vq)
{
	return struct_size_t(struct vduse_guest_log_queue, desc, vq->size);
}

static void vduse_guest_clear_log(struct vduse_guest_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++) {
		struct vduse_guest_vq *vq = &dev->vqs[i];

		/* The size stays, so that a crash here leaves a valid log. */
		vq->log->used_idx = 0;
		vq->log->last_head = 0;
		memset(vq->log->desc, 0, vq->size * sizeof(*vq->log->desc));
		vq->counter = 0;
		vduse_guest_free_replay(vq);
	}
}

struct vduse_guest_inflight {
	u64 counter;
	u16 head;
};

static int vduse_guest_cmp_inflight(const void *a, const void *b)
{
	const struct vduse_guest_inflight *x = a, *y = b;

	return x->counter < y->counter ? -1 : x->counter > y->counter;
}

/*
 * Take over a queue that an earlier guest left running. The log marks its
 * outstanding requests, and the next entry of the available ring follows
 * them, since every request taken is either completed or in flight. This
 * runs again after a failed start, so it replaces any earlier replay list.
 */
static int vduse_guest_recover_vq(struct vduse_guest_dev *dev,
				  struct vduse_guest_vq *vq)
{
	struct vduse_guest_log_queue *log = vq->log;
	unsigned int num = vq->vrh.vring.num;
	struct vduse_guest_inflight *in;
	unsigned int i, n = 0;
	u16 used;

	vduse_guest_free_replay(vq);
	vq->counter = 0;

	used = vduse_guest_used_idx(vq);
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

	for (i = 0; i < vq->size; i++) {
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
	sort(in, n, sizeof(*in), vduse_guest_cmp_inflight, NULL);

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

static int vduse_guest_start_vq(struct vduse_guest_dev *dev,
				struct vduse_guest_vq *vq)
{
	struct vduse_guest_vq_info info;
	void *desc, *avail, *used;
	size_t num;
	int err;

	err = dev->hops->vq_get_info(dev, vq->index, &info);
	if (err)
		return err;
	vq->ready = info.ready;
	if (!vq->ready)
		return 0;

	/* The queue's area of the log has an entry for each possible head. */
	num = info.num;
	if (!num || num > vq->size)
		return -EINVAL;
	desc = vduse_guest_translate(dev, info.desc_addr,
				     num * sizeof(struct vring_desc), false);
	avail = vduse_guest_translate(dev, info.driver_addr,
				      sizeof(struct vring_avail) +
				      (num + 1) * sizeof(__virtio16), false);
	used = vduse_guest_translate(dev, info.device_addr,
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
		err = vduse_guest_recover_vq(dev, vq);
		if (err)
			return err;
	} else {
		WRITE_ONCE(vq->log->used_idx, vduse_guest_used_idx(vq));
	}

	return dev->hops->enable_kick(dev, vq->index);
}

static void vduse_guest_stop_vqs(struct vduse_guest_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++) {
		WRITE_ONCE(dev->vqs[i].running, false);
		dev->hops->sync_kick(dev, i);
	}
}

static int vduse_guest_start(struct vduse_guest_dev *dev)
{
	unsigned int i;
	int err;

	err = dev->hops->get_features(dev, &dev->features);
	if (err)
		return err;

	for (i = 0; i < dev->num_queues; i++) {
		err = vduse_guest_start_vq(dev, &dev->vqs[i]);
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
			vduse_guest_inject(dev, i);
		dev->ops->kick(dev->priv, i);
	}
	dev->reconnect = false;

	return 0;
}

static void vduse_guest_stop(struct vduse_guest_dev *dev)
{
	vduse_guest_stop_vqs(dev);
	dev->ops->stop(dev->priv);
	dev->running = false;
}

/*
 * The host frees and reallocates the rings and the bounce region without a
 * message, so a reset drops every mapping.
 *
 * A failed start is answered as a success. VDUSE records DRIVER_OK only when
 * the device accepts it, and injects no interrupt without it, while the
 * host's driver submits requests either way. Those requests would then never
 * complete, and the host's driver would hang. Accepted, they stay outstanding
 * until a guest that takes over the device, or scripts/vduse-blk-drain.py,
 * completes them.
 */
static void vduse_guest_set_status(struct vduse_guest_dev *dev, u8 status)
{
	int err;

	if ((status & VIRTIO_CONFIG_S_DRIVER_OK) && !dev->running) {
		err = vduse_guest_start(dev);
		if (err) {
			pr_err("%s: cannot start the device, so its requests wait for another guest: %d\n",
			       dev->name, err);
			atomic64_inc(&dev->stats.start_errors);
		} else {
			atomic64_inc(&dev->stats.starts);
		}
	} else if (!(status & VIRTIO_CONFIG_S_DRIVER_OK) && dev->running) {
		vduse_guest_stop(dev);
	}

	if (!status) {
		vduse_guest_unmap_range(dev, 0, U64_MAX);
		vduse_guest_clear_log(dev);
		dev->reconnect = false;
		atomic64_inc(&dev->stats.resets);
	}
}

/*
 * Start the queues that the driver had running when the earlier guest
 * stopped. With no ready queue, the driver has not started the device, and
 * its messages start it as usual. If the start fails, the device stays
 * reconnecting, so that a later DRIVER_OK takes over the queues again
 * instead of starting them from the beginning.
 */
static void vduse_guest_resume(struct vduse_guest_dev *dev)
{
	struct vduse_guest_vq_info info;
	bool ready = false;
	unsigned int i;
	int err;

	dev->resume = false;
	atomic64_inc(&dev->stats.reconnects);

	for (i = 0; i < dev->num_queues; i++) {
		if (!dev->hops->vq_get_info(dev, i, &info) && info.ready)
			ready = true;
	}

	if (!ready) {
		dev->reconnect = false;
		return;
	}

	err = vduse_guest_start(dev);
	if (err) {
		pr_err("%s: cannot resume the device: %d\n", dev->name, err);
		atomic64_inc(&dev->stats.start_errors);
	} else {
		atomic64_inc(&dev->stats.starts);
	}
}

static void vduse_guest_msg_work(struct work_struct *work)
{
	struct vduse_guest_dev *dev = container_of(work, struct vduse_guest_dev,
						   msg_work);
	struct vduse_guest_msg msg;
	u16 avail_index;
	bool ok;
	int err;

	/*
	 * A reconnecting device takes over its queues before handling the
	 * messages that the earlier guest left unanswered.
	 */
	if (dev->resume)
		vduse_guest_resume(dev);

	while (!(err = dev->hops->read_msg(dev, &msg))) {
		avail_index = 0;
		ok = true;
		atomic64_inc(&dev->stats.msgs[msg.type]);

		switch (msg.type) {
		case VDUSE_GUEST_MSG_GET_VQ_STATE:
			if (msg.index < dev->num_queues)
				avail_index = dev->vqs[msg.index].vrh.last_avail_idx;
			else
				ok = false;
			break;
		case VDUSE_GUEST_MSG_SET_STATUS:
			vduse_guest_set_status(dev, msg.status);
			break;
		case VDUSE_GUEST_MSG_UPDATE_IOTLB:
			/* The rings stay mapped while the device runs. */
			if (!dev->running)
				vduse_guest_unmap_range(dev, msg.start, msg.last);
			break;
		default:
			ok = false;
			break;
		}

		err = dev->hops->reply(dev, &msg, ok, avail_index);
		if (err)
			pr_warn("%s: cannot answer a message: %d\n", dev->name,
				err);
	}

	if (err != -EAGAIN)
		pr_warn("%s: cannot read a message: %d\n", dev->name, err);
}

void vduse_guest_msg_event(struct vduse_guest_dev *dev)
{
	schedule_work(&dev->msg_work);
}

void vduse_guest_kick(struct vduse_guest_dev *dev, unsigned int index,
		      u64 count)
{
	struct vduse_guest_vq *vq = &dev->vqs[index];

	atomic64_inc(&vq->stats.kick_irqs);
	if (!count)
		return;

	atomic64_add(count, &vq->stats.kicks);
	if (READ_ONCE(vq->running))
		dev->ops->kick(dev->priv, index);
	else
		atomic64_add(count, &vq->stats.dropped_kicks);
}

static u64 __init vduse_guest_features(const struct vduse_guest_config *cfg)
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
static int __init vduse_guest_create_host(struct vduse_guest_dev *dev,
					  const struct vduse_guest_config *cfg)
{
	u64 features = vduse_guest_features(cfg);
	int err;

	err = dev->hops->create(dev, cfg, features, PAGE_SIZE);
	if (err != -EEXIST)
		return err;

	err = dev->hops->destroy(dev);
	if (err == -EBUSY)
		return 1;
	if (err)
		return err;

	return dev->hops->create(dev, cfg, features, PAGE_SIZE);
}

/*
 * Check that a device left by an earlier guest has this device's queues, and
 * that its driver uses no feature this device does not offer. VDUSE cannot
 * report the device type or configuration space, so those are assumed to be
 * the same.
 */
static int __init vduse_guest_check_reconnect(struct vduse_guest_dev *dev,
					      u64 offered)
{
	struct vduse_guest_vq_info info;
	unsigned int i;
	u64 features;
	int err;

	err = dev->hops->get_features(dev, &features);
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
		err = dev->hops->vq_get_info(dev, i, &info);
		if (err || info.num > dev->vqs[i].size)
			goto mismatch;
	}
	if (!dev->hops->vq_get_info(dev, dev->num_queues, &info))
		goto mismatch;
	return 0;

mismatch:
	pr_err("%s: the device has different queues from this one\n",
	       dev->name);
	return -EINVAL;
}

/*
 * Each queue's area is checked only after the areas before it have matched,
 * since a different size would move every area after it.
 */
static bool __init vduse_guest_log_matches(struct vduse_guest_dev *dev,
					   u64 node_dev, u64 node_ino)
{
	struct vduse_guest_log_header *hdr = dev->log;
	unsigned int i;

	if (hdr->magic != VDUSE_GUEST_LOG_MAGIC ||
	    hdr->version != VDUSE_GUEST_LOG_VERSION ||
	    hdr->num_queues != dev->num_queues ||
	    hdr->node_dev != node_dev || hdr->node_ino != node_ino)
		return false;

	for (i = 0; i < dev->num_queues; i++)
		if (dev->vqs[i].log->size != dev->vqs[i].size)
			return false;
	return true;
}

/*
 * Map the in-flight log. A new device starts with an empty log. Reconnecting
 * needs the log of the earlier guest, since without it the requests that
 * guest left outstanding cannot be found.
 */
static int __init vduse_guest_open_log(struct vduse_guest_dev *dev)
{
	struct vduse_guest_log_header *hdr;
	u64 node_dev, node_ino;
	size_t offset;
	unsigned int i;
	int err;

	err = dev->hops->node_id(dev, &node_dev, &node_ino);
	if (err)
		return err;

	dev->log_size = sizeof(*hdr);
	for (i = 0; i < dev->num_queues; i++)
		dev->log_size += vduse_guest_log_queue_size(&dev->vqs[i]);

	err = dev->hops->map_log(dev, dev->log_size, !dev->reconnect,
				 &dev->log);
	if (err == -EINVAL) {
		pr_err("%s: the in-flight log does not describe this device\n",
		       dev->name);
		return -EINVAL;
	}
	if (err)
		return err;

	offset = sizeof(*hdr);
	for (i = 0; i < dev->num_queues; i++) {
		dev->vqs[i].log = dev->log + offset;
		offset += vduse_guest_log_queue_size(&dev->vqs[i]);
	}

	if (dev->reconnect) {
		if (!vduse_guest_log_matches(dev, node_dev, node_ino)) {
			pr_err("%s: the in-flight log does not describe this device\n",
			       dev->name);
			return -EINVAL;
		}
		return 0;
	}

	memset(dev->log, 0, dev->log_size);
	hdr = dev->log;
	hdr->magic = VDUSE_GUEST_LOG_MAGIC;
	hdr->version = VDUSE_GUEST_LOG_VERSION;
	hdr->num_queues = dev->num_queues;
	hdr->node_dev = node_dev;
	hdr->node_ino = node_ino;
	for (i = 0; i < dev->num_queues; i++)
		dev->vqs[i].log->size = dev->vqs[i].size;
	return 0;
}

static void __init vduse_guest_close_log(struct vduse_guest_dev *dev)
{
	unsigned int i;

	for (i = 0; i < dev->num_queues; i++)
		dev->vqs[i].log = NULL;
	if (dev->log)
		dev->hops->unmap_log(dev, dev->log, dev->log_size);
	dev->log = NULL;
}

static void vduse_guest_close_vqs(struct vduse_guest_dev *dev, unsigned int n)
{
	while (n--)
		dev->hops->close_vq(dev, n);
}

/* A reconnecting guest keeps the sizes that the device already has. */
static int __init vduse_guest_open_vqs(struct vduse_guest_dev *dev)
{
	unsigned int i;
	int err;

	for (i = 0; i < dev->num_queues; i++) {
		if (!dev->reconnect) {
			err = dev->hops->vq_setup(dev, i, dev->vqs[i].size);
			if (err)
				goto close;
		}

		/*
		 * Each queue's kicks, and the backend's work that they
		 * schedule, run on a CPU of its own where there are enough.
		 */
		err = dev->hops->open_vq(dev, i,
					 cpumask_local_spread(i, NUMA_NO_NODE));
		if (err)
			goto close;
	}

	return 0;

close:
	vduse_guest_close_vqs(dev, i);
	return err;
}

static const struct vduse_guest_backend_ops *__init
vduse_guest_find_backend(const char *type)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(vduse_guest_backends); i++) {
		if (!strcmp(vduse_guest_backends[i]->type, type))
			return vduse_guest_backends[i];
	}
	return NULL;
}

static int __init vduse_guest_setup(struct vduse_guest_dev *dev)
{
	struct vduse_guest_config cfg = {};
	unsigned int i;
	int err;

	dev->ops = vduse_guest_find_backend(dev->type);
	if (!dev->ops)
		return -ENODEV;

	dev->priv = dev->ops->setup(dev, dev->opts, &cfg);
	if (IS_ERR(dev->priv))
		return PTR_ERR(dev->priv);

	if (!cfg.num_queues || !cfg.queue_sizes)
		return -EINVAL;
	for (i = 0; i < cfg.num_queues; i++)
		if (!is_power_of_2(cfg.queue_sizes[i]))
			return -EINVAL;

	dev->num_queues = cfg.num_queues;
	dev->vqs = kzalloc_objs(*dev->vqs, dev->num_queues);
	if (!dev->vqs)
		return -ENOMEM;

	for (i = 0; i < dev->num_queues; i++) {
		dev->vqs[i].dev = dev;
		dev->vqs[i].index = i;
		dev->vqs[i].size = cfg.queue_sizes[i];
		spin_lock_init(&dev->vqs[i].lock);
	}

	err = vduse_guest_create_host(dev, &cfg);
	if (err < 0)
		return err;
	dev->reconnect = dev->resume = err;

	err = dev->hops->open(dev);
	if (err) {
		if (err == -EBUSY)
			pr_err("%s: another process serves the device\n",
			       dev->name);
		goto destroy;
	}

	/* Once the open succeeds, no other process serves the device. */
	if (dev->reconnect && !cfg.allow_reconnect) {
		pr_err("%s: an earlier guest left the device in use, and a %s device cannot take it over, run 'vdpa dev del %s' on the host\n",
		       dev->name, dev->type, dev->name);
		err = -EBUSY;
		goto close;
	}

	if (dev->reconnect) {
		err = vduse_guest_check_reconnect(dev,
						  vduse_guest_features(&cfg));
		if (err)
			goto close;
	}

	err = vduse_guest_open_log(dev);
	if (err)
		goto close;

	err = vduse_guest_open_vqs(dev);
	if (err)
		goto close;

	err = dev->hops->enable_msgs(dev);
	if (err) {
		vduse_guest_close_vqs(dev, dev->num_queues);
		goto close;
	}

	pr_info("%s: %s %s device\n", dev->name,
		dev->reconnect ? "reconnecting to the" : "created", dev->type);

	/*
	 * Messages sent before they were enabled raise no event. A
	 * reconnecting device resumes in the work too.
	 */
	schedule_work(&dev->msg_work);
	return 0;

close:
	vduse_guest_close_log(dev);
	dev->hops->close(dev);
destroy:
	if (!dev->reconnect)
		dev->hops->destroy(dev);
	return err;
}

/* Take the transport's own options and the host's out of the backend's. */
static int __init vduse_guest_parse_opts(struct vduse_guest_dev *dev,
					 char *opts)
{
	size_t len = 0;
	char *opt;
	int r;

	dev->opts = kzalloc(opts ? strlen(opts) + 1 : 1, GFP_KERNEL);
	if (!dev->opts)
		return -ENOMEM;

	while ((opt = strsep(&opts, ","))) {
		if (!strcmp(opt, "bounce=guest")) {
			dev->bounce_guest = true;
		} else if (!strcmp(opt, "bounce=host")) {
			dev->bounce_guest = false;
		} else if (strstarts(opt, "poll=")) {
			unsigned int usecs;

			if (kstrtouint(opt + strlen("poll="), 10, &usecs))
				return -EINVAL;
			dev->poll_ns = (u64)usecs * NSEC_PER_USEC;
		} else if (*opt) {
			r = dev->hops->parse_opt(dev, opt);
			if (r < 0)
				return r;
			if (r)
				continue;
			if (len)
				dev->opts[len++] = ',';
			strcpy(dev->opts + len, opt);
			len += strlen(opt);
		}
	}

	return dev->hops->check_opts(dev);
}

/* Add a device given as <type>,<name>[,<options>]. */
static void __init vduse_guest_add(const char *arg)
{
	struct vduse_guest_host *host;
	struct vduse_guest_dev *dev;
	char *sep;
	int err;

	dev = kzalloc_obj(*dev);
	if (!dev)
		return;

	INIT_LIST_HEAD(&dev->regions);
	mutex_init(&dev->map_lock);
	mutex_init(&dev->umem_lock);
	INIT_WORK(&dev->msg_work, vduse_guest_msg_work);

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

	host = vduse_guest_find_host(dev->name);
	if (!host) {
		pr_err("%s: no host serves the device\n", dev->name);
		goto free;
	}
	dev->hops = host->ops;

	err = dev->hops->alloc(dev, host);
	if (err) {
		pr_err("%s: cannot create the %s device: %d\n", dev->name,
		       dev->type, err);
		goto free;
	}

	err = vduse_guest_parse_opts(dev, sep ? sep + 1 : NULL);
	if (err) {
		pr_err("%s: invalid options\n", dev->name);
		goto free_host;
	}

	err = vduse_guest_setup(dev);
	if (err) {
		pr_err("%s: cannot create the %s device: %d\n", dev->name,
		       dev->type, err);
		if (dev->ops && !IS_ERR_OR_NULL(dev->priv))
			dev->ops->release(dev->priv);
		goto free_host;
	}

	list_add_tail(&dev->list, &vduse_guest_devices);
	return;

free_host:
	dev->hops->free(dev);
free:
	kfree(dev->opts);
	kfree(dev->vqs);
	kfree(dev->type);
	kfree(dev);
}

static int vduse_guest_cmdline_set(const char *val,
				   const struct kernel_param *kp)
{
	if (vduse_guest_nr_args == VDUSE_GUEST_MAX_DEVICES)
		return -ENOSPC;
	vduse_guest_args[vduse_guest_nr_args++] = val;
	return 0;
}

static int vduse_guest_cmdline_get(char *buffer,
				   const struct kernel_param *kp)
{
	return 0;
}

static const struct kernel_param_ops vduse_guest_cmdline_param_ops = {
	.set = vduse_guest_cmdline_set,
	.get = vduse_guest_cmdline_get,
};

device_param_cb(device, &vduse_guest_cmdline_param_ops, NULL, 0400);

static const char *const vduse_guest_msg_names[] = {
	[VDUSE_GUEST_MSG_GET_VQ_STATE] = "msg_get_vq_state",
	[VDUSE_GUEST_MSG_SET_STATUS] = "msg_set_status",
	[VDUSE_GUEST_MSG_UPDATE_IOTLB] = "msg_update_iotlb",
	[VDUSE_GUEST_MSG_UNKNOWN] = "msg_unknown",
};

static void vduse_guest_show_counter(struct seq_file *m, int indent,
				     const char *name, atomic64_t *v)
{
	seq_printf(m, "%*s%-*s %lld\n", indent, "", 20 - indent, name,
		   atomic64_read(v));
}

static void vduse_guest_show_device(struct seq_file *m,
				    struct vduse_guest_dev *dev)
{
	struct vduse_guest_dev_stats *stats = &dev->stats;
	unsigned int i;

	seq_printf(m, "device %s\n", dev->name);
	seq_printf(m, "  %-18s %s\n", "type", dev->type);
	seq_printf(m, "  %-18s %s\n", "state",
		   READ_ONCE(dev->running) ? "running" : "stopped");
	seq_printf(m, "  %-18s %s\n", "bounce_region",
		   READ_ONCE(dev->umem_registered) ? "guest" : "host");
	seq_printf(m, "  %-18s 0x%llx\n", "features", READ_ONCE(dev->features));

	for (i = 0; i < ARRAY_SIZE(vduse_guest_msg_names); i++)
		vduse_guest_show_counter(m, 2, vduse_guest_msg_names[i],
					 &stats->msgs[i]);
	vduse_guest_show_counter(m, 2, "starts", &stats->starts);
	vduse_guest_show_counter(m, 2, "start_errors", &stats->start_errors);
	vduse_guest_show_counter(m, 2, "resets", &stats->resets);
	vduse_guest_show_counter(m, 2, "iova_maps", &stats->maps);
	vduse_guest_show_counter(m, 2, "iova_unmaps", &stats->unmaps);
	vduse_guest_show_counter(m, 2, "iova_map_errors", &stats->map_errors);
	vduse_guest_show_counter(m, 2, "reconnects", &stats->reconnects);
	vduse_guest_show_counter(m, 2, "umem_regs", &stats->umem_regs);
	if (dev->ops->show)
		dev->ops->show(dev->priv, m);

	for (i = 0; i < dev->num_queues; i++) {
		struct vduse_guest_vq_stats *q = &dev->vqs[i].stats;

		seq_printf(m, "  queue %u\n", i);
		vduse_guest_show_counter(m, 4, "kicks", &q->kicks);
		vduse_guest_show_counter(m, 4, "kick_irqs", &q->kick_irqs);
		vduse_guest_show_counter(m, 4, "dropped_kicks",
					 &q->dropped_kicks);
		vduse_guest_show_counter(m, 4, "requests", &q->requests);
		vduse_guest_show_counter(m, 4, "bad_requests",
					 &q->bad_requests);
		vduse_guest_show_counter(m, 4, "out_bytes", &q->out_bytes);
		vduse_guest_show_counter(m, 4, "in_bytes", &q->in_bytes);
		vduse_guest_show_counter(m, 4, "injected", &q->injected);
		vduse_guest_show_counter(m, 4, "suppressed", &q->suppressed);
		vduse_guest_show_counter(m, 4, "inject_errors",
					 &q->inject_errors);
		vduse_guest_show_counter(m, 4, "replayed", &q->replayed);
		vduse_guest_show_counter(m, 4, "polled", &q->polled);
		vduse_guest_show_counter(m, 4, "poll_timeouts",
					 &q->poll_timeouts);
	}
}

static int __maybe_unused vduse_guest_proc_show(struct seq_file *m, void *v)
{
	struct vduse_guest_dev *dev;

	list_for_each_entry(dev, &vduse_guest_devices, list)
		vduse_guest_show_device(m, dev);

	return 0;
}

/*
 * Stop the backend, which then makes no more calls on the queues, disconnect
 * from the device and destroy it on the host. A device that is still on the
 * host's vDPA bus cannot be destroyed, and the next guest reconnects to it.
 */
static void vduse_guest_release(struct vduse_guest_dev *dev)
{
	int err;

	/* Once messages are off, none can start the backend again. */
	dev->hops->disable_msgs(dev);
	cancel_work_sync(&dev->msg_work);

	if (dev->running)
		vduse_guest_stop(dev);
	vduse_guest_unmap_range(dev, 0, U64_MAX);
	vduse_guest_close_vqs(dev, dev->num_queues);
	dev->hops->close(dev);
	/* Closing the device made VDUSE drop its pins on these pages. */
	vduse_guest_free_umem(dev);

	/* A device still on the bus keeps its log for the next guest. */
	err = dev->hops->destroy(dev);
	if (err == -EBUSY) {
		pr_info("%s: the device stays on the host's vDPA bus for the next guest\n",
			dev->name);
	} else if (err) {
		pr_warn("%s: cannot destroy the device: %d\n", dev->name, err);
	} else {
		pr_info("%s: destroyed the device\n", dev->name);
		dev->hops->drop_log(dev);
	}
}

/* This runs before device_shutdown(), while the backends' disks still work. */
static int vduse_guest_reboot(struct notifier_block *nb, unsigned long code,
			      void *unused)
{
	struct vduse_guest_dev *dev;

	list_for_each_entry(dev, &vduse_guest_devices, list)
		vduse_guest_release(dev);

	return NOTIFY_DONE;
}

static struct notifier_block vduse_guest_reboot_notifier = {
	.notifier_call = vduse_guest_reboot,
};

static int __init vduse_guest_init(void)
{
	int i;

	if (!vduse_guest_nr_args)
		return 0;

	for (i = 0; i < vduse_guest_nr_args; i++)
		vduse_guest_add(vduse_guest_args[i]);

	proc_create_single("vduse_guest", 0444, NULL, vduse_guest_proc_show);
	register_reboot_notifier(&vduse_guest_reboot_notifier);
	return 0;
}
late_initcall(vduse_guest_init);
