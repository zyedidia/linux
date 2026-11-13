// SPDX-License-Identifier: GPL-2.0
/*
 * The VDUSE transport.
 *
 * The state that the host reads, or that decides what it reads, is protected
 * as prot.h describes: the rings, the regions that translate IOVAs, the vringh
 * state of each queue, the in-flight log and the translation of the bounce
 * region. Backends and the message work reach it only through protected
 * functions, which check what the rest of the guest gives them. A completion
 * must name a head that a backend holds, with a used length of at most the
 * writable length of its chain.
 */

#define pr_fmt(fmt) "vduse-guest: " fmt

#include <linux/atomic.h>
#include <linux/cache.h>
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
#include <linux/virtio_byteorder.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <asm/sections.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ring.h>

#include "prot.h"

#define VDUSE_GUEST_MAX_DEVICES 8
#define VDUSE_GUEST_LOG_MAGIC 0x676f6c76
#define VDUSE_GUEST_LOG_VERSION 3

/* The most buffers that a chain has in each direction. */
#define VDUSE_GUEST_IOV_MAX 1024

/* The used length limit of a head that no backend holds. */
#define VDUSE_GUEST_NOT_TAKEN U32_MAX

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
	/* The used index of the last completion that was logged. */
	u16 used_idx;
	u8 pad0[2];
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
 * because the driver does not want one. Bad_completions counts completions
 * that were dropped because they named a head that no backend held or a
 * used length beyond the chain's writable buffers.
 */
struct vduse_guest_vq_stats {
	atomic64_t kicks;
	atomic64_t kick_irqs;
	atomic64_t dropped_kicks;
	atomic64_t requests;
	atomic64_t bad_requests;
	atomic64_t bad_completions;
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
	/* The IOVA and the error of the last map that failed. */
	u64 map_error_iova;
	int map_error;
};

/*
 * A queue as the rest of the guest sees it. Running mirrors the protected
 * state for kicks, and replay gives the requests that the last start found to
 * replay.
 */
struct vduse_guest_vq {
	u16 size;
	bool running;
	unsigned int replay;
	struct vduse_guest_vq_stats stats;
};

/*
 * The protected state of a queue. The avail lock serializes taking chains
 * from the available ring with the queue's start and reset, and the used
 * lock serializes writes to the used ring and the log. Started is set while
 * the rings are mapped, running while backends may take chains, and ran
 * from when they first may until the next reset.
 *
 * A chain is pending from when it is taken until a backend gets it, which
 * waits while the backend's iovs are too small or while the bounce region
 * waits for guest pages. Walked is set once its buffers are in riov and
 * wiov, and replayed if an earlier guest took it. Taken gives the writable
 * length of each head that a backend holds.
 */
struct vduse_guest_pvq {
	struct vduse_guest_prot *p;
	struct vduse_guest_vq *vq;
	unsigned int index;
	/* The largest size that the driver may give the queue. */
	u16 size;
	struct vringh vrh;
	struct vduse_prot_mutex avail_lock;
	struct vduse_prot_lock used_lock;
	bool started;
	bool ready;
	bool running;
	bool ran;

	bool pending;
	bool walked;
	bool replayed;
	bool need_umem;
	bool skip_umem;
	u16 pending_head;
	struct vringh_kiov riov;
	struct vringh_kiov wiov;

	u32 *taken;
	struct vring_used_elem *used;

	/* The queue's area of the in-flight log. */
	struct vduse_guest_log_queue *log;
	u64 counter;
	/* Requests of an earlier guest to take again, oldest first. */
	u16 *replay;
	unsigned int nr_replay;
	unsigned int replay_pos;
};

/*
 * The protected state of a device. The life lock serializes its start,
 * reset and release. The regions are the parts of the IOVA space that are
 * mapped into the guest, under map_lock, and the regions that hold rings are
 * protected.
 *
 * Reconnect is set when the device was left by a guest, until a start takes
 * over its queues. With guest pages behind the bounce region, the address of
 * each chunk is in umem_addr. Umem_wanted is set from a start until the rest
 * of the guest has tried to register them, and translations wait for it.
 */
struct vduse_guest_prot {
	struct vduse_guest_dev *dev;
	const struct vduse_guest_host_ops *hops;
	struct vduse_guest_dev_stats *stats;
	unsigned int num_queues;
	struct vduse_guest_pvq *vqs;
	struct vduse_prot_mutex life_lock;
	u64 features;
	bool running;
	bool reconnect;
	bool bounce_guest;

	/* The in-flight log, which is protected memory if log_protected. */
	void *log;
	size_t log_size;
	bool log_protected;

	struct list_head regions;
	struct vduse_prot_mutex map_lock;

	bool umem_wanted;
	bool umem_registered;
	u64 umem_size;
	unsigned int umem_shift;
	unsigned int umem_nr;
	unsigned long *umem_addr;
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
	/*
	 * The fault requests that the backend has taken and not answered,
	 * which the release waits for before it closes the device.
	 */
	atomic_t shm_faults;
	wait_queue_head_t shm_faults_wq;

	/* The device's index in vduse_guest_prots. */
	unsigned int slot;
	unsigned int num_queues;
	struct vduse_guest_vq *vqs;

	/*
	 * The features that the host's driver accepted, and whether the rings
	 * are little-endian, as the last start found them.
	 */
	u64 features;
	bool ring_le;
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

	/* The size of the in-flight log, once it is mapped. */
	size_t log_size;

	/*
	 * Guest pages that back the bounce region, so that backends get guest
	 * pages for the host's buffers. The host hands chunks of contiguous
	 * pages to VDUSE. Umem_lock serializes the registration, and
	 * umem_wanted asks for one. Bounce_guest is set by bounce=guest,
	 * without which the region stays in host memory.
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

/*
 * The protected state of each device, which is fixed after boot, so that the
 * rest of the guest cannot point a device at other state.
 */
static struct vduse_guest_prot *
vduse_guest_prots[VDUSE_GUEST_MAX_DEVICES] __ro_after_init;
static unsigned int vduse_guest_nr_prots __initdata;

static const struct vduse_guest_backend_ops *const vduse_guest_backends[] = {
#ifdef CONFIG_VDUSE_GUEST_BLK
	&vduse_guest_blk_ops,
#endif
#ifdef CONFIG_VDUSE_GUEST_WLAN
	&vduse_guest_wlan_ops,
#endif
#ifdef CONFIG_VDUSE_GUEST_NET
	&vduse_guest_net_ops,
#endif
#ifdef CONFIG_VDUSE_GUEST_GPU
	&vduse_guest_gpu_ops,
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

u64 vduse_guest_driver_features(struct vduse_guest_dev *dev)
{
	return dev->features;
}

/* Hosts without configuration updates return -EOPNOTSUPP. */
int vduse_guest_set_config(struct vduse_guest_dev *dev, u32 offset,
			   const void *buf, u32 len)
{
	if (!dev->hops->set_config)
		return -EOPNOTSUPP;
	return dev->hops->set_config(dev, offset, buf, len);
}

/*
 * Raise the configuration interrupt. VDUSE refuses it with -EINVAL until the
 * driver's DRIVER_OK has been answered.
 */
int vduse_guest_config_irq(struct vduse_guest_dev *dev)
{
	if (!dev->hops->inject_config)
		return -EOPNOTSUPP;
	return dev->hops->inject_config(dev);
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

/* The protected state of a device, or NULL for a device that it is not. */
static struct vduse_guest_prot *vduse_guest_prot_of(struct vduse_guest_dev *dev)
{
	unsigned int slot = READ_ONCE(dev->slot);
	struct vduse_guest_prot *p;

	if (slot >= ARRAY_SIZE(vduse_guest_prots))
		return NULL;
	p = vduse_guest_prots[slot];
	return p && p->dev == dev ? p : NULL;
}

static struct vduse_guest_pvq *vduse_guest_pvq_of(struct vduse_guest_dev *dev,
						   unsigned int index)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p || index >= p->num_queues)
		return NULL;
	return &p->vqs[index];
}

static void vduse_guest_unmap_region(struct vduse_guest_prot *p,
				     struct vduse_guest_mapping *m)
{
	p->hops->unmap(p->dev, &m->region);
	list_del(&m->list);
	vduse_prot_free(m);
	atomic64_inc(&p->stats->unmaps);
}

/*
 * Find the region that holds an IOVA, and map it on a miss, with map_lock
 * held. A region for rings is protected, and a buffer in one is refused. A
 * region that was mapped for buffers is mapped again when it turns out to
 * hold rings. Regions are only unmapped while the queues are stopped, so a
 * region stays valid after the lock is dropped.
 */
static struct vduse_guest_mapping *
__vduse_guest_find_region(struct vduse_guest_prot *p, u64 iova, bool protect)
{
	struct vduse_guest_mapping *m;
	int err;

	protect = protect && vduse_prot_active();
	list_for_each_entry(m, &p->regions, list) {
		if (iova < m->region.start || iova > m->region.last)
			continue;
		if (m->region.protect == protect)
			return m;
		if (!protect)
			return NULL;
		vduse_guest_unmap_region(p, m);
		break;
	}

	m = vduse_prot_alloc(sizeof(*m));
	if (!m)
		return NULL;
	m->region.protect = protect;
	err = p->hops->map(p->dev, iova, &m->region);
	if (!err && (m->region.protect != protect ||
		     (!protect && !vduse_prot_outside(m->region.addr,
						      m->region.last -
						      m->region.start + 1)))) {
		p->hops->unmap(p->dev, &m->region);
		err = -EFAULT;
	}
	if (err) {
		WRITE_ONCE(p->stats->map_error_iova, iova);
		WRITE_ONCE(p->stats->map_error, err);
		atomic64_inc(&p->stats->map_errors);
		vduse_prot_free(m);
		return NULL;
	}
	list_add(&m->list, &p->regions);
	atomic64_inc(&p->stats->maps);
	return m;
}

static struct vduse_guest_mapping *
vduse_guest_find_region(struct vduse_guest_prot *p, u64 iova, bool protect)
{
	struct vduse_guest_mapping *m;

	vduse_prot_mutex_lock(&p->map_lock);
	m = __vduse_guest_find_region(p, iova, protect);
	vduse_prot_mutex_unlock(&p->map_lock);
	return m;
}

static void *vduse_guest_translate(struct vduse_guest_prot *p, u64 iova,
				   size_t len, bool write)
{
	struct vduse_guest_mapping *m;
	u64 last;

	if (!len || check_add_overflow(iova, (u64)len - 1, &last))
		return NULL;

	m = vduse_guest_find_region(p, iova, true);
	if (!m || last > m->region.last)
		return NULL;
	if (write ? !m->region.writable : !m->region.readable)
		return NULL;
	return m->region.addr + (iova - m->region.start);
}

/*
 * Find the region that holds a device address. The host maps the bounce
 * region and coherent allocations for both reading and writing, so a range
 * covers a whole region. With guest pages behind it, the bounce region is
 * one range a chunk.
 */
static bool vduse_guest_range(struct vduse_guest_prot *p, u64 addr,
			      struct vringh_range *range)
{
	struct vduse_guest_mapping *m;
	bool found = true;

	vduse_prot_mutex_lock(&p->map_lock);
	if (p->umem_registered && addr < p->umem_size) {
		u64 chunk = addr >> p->umem_shift;

		range->start = chunk << p->umem_shift;
		range->end_incl = min(range->start + BIT_ULL(p->umem_shift),
				      p->umem_size) - 1;
		range->offset = p->umem_addr[chunk] - range->start;
	} else if ((m = __vduse_guest_find_region(p, addr, false))) {
		range->start = m->region.start;
		range->end_incl = m->region.last;
		range->offset = (unsigned long)m->region.addr - m->region.start;
	} else {
		found = false;
	}
	vduse_prot_mutex_unlock(&p->map_lock);
	return found;
}

/*
 * Give vringh the region that holds a buffer or an indirect table. While the
 * bounce region waits for guest pages, the walk stops and asks for them.
 */
static bool vduse_guest_getrange(struct vringh *vrh, u64 addr,
				 struct vringh_range *range)
{
	struct vduse_guest_pvq *pvq = container_of(vrh, struct vduse_guest_pvq,
						   vrh);

	if (pvq->p->umem_wanted && !pvq->skip_umem) {
		pvq->need_umem = true;
		return false;
	}
	return vduse_guest_range(pvq->p, addr, range);
}

static long __vduse_prot vduse_guest_p_read(struct vduse_guest_dev *dev,
					    u64 addr, void *buf, size_t len,
					    bool skip_umem)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	struct vringh_range range;
	size_t n;

	if (!p)
		return -EINVAL;
	if (!vduse_prot_outside(buf, len))
		return -EFAULT;
	if (p->umem_wanted && !skip_umem)
		return -EAGAIN;

	while (len) {
		if (!vduse_guest_range(p, addr, &range))
			return -EFAULT;
		n = min_t(u64, len - 1, range.end_incl - addr) + 1;
		memcpy(buf, (void *)(unsigned long)(addr + range.offset), n);
		addr += n;
		buf += n;
		len -= n;
	}
	return 0;
}

static void vduse_guest_free_umem_table(struct vduse_guest_prot *p)
{
	p->umem_registered = false;
	vduse_prot_free(p->umem_addr);
	p->umem_addr = NULL;
	p->umem_nr = 0;
}

static long __vduse_prot vduse_guest_p_drop_umem(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->map_lock);
	vduse_guest_free_umem_table(p);
	vduse_prot_mutex_unlock(&p->map_lock);
	return 0;
}

/*
 * End a registration of guest pages for the bounce region, with the address
 * of each chunk if it succeeded. Translations stop waiting for it either
 * way.
 */
static long __vduse_prot vduse_guest_p_umem_done(struct vduse_guest_dev *dev,
						 const unsigned long *addrs,
						 unsigned int nr,
						 unsigned int shift, u64 size)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	unsigned long *table = NULL;
	unsigned int i;
	long err = 0;

	if (!p)
		return -EINVAL;

	if (nr) {
		if (shift < PAGE_SHIFT || shift >= 32 || !size ||
		    nr != DIV_ROUND_UP(size, BIT_ULL(shift)) ||
		    !vduse_prot_outside(addrs, nr * sizeof(*addrs)))
			err = -EINVAL;
		if (!err)
			table = vduse_prot_alloc_array(nr, sizeof(*table));
		if (!err && !table)
			err = -ENOMEM;
		for (i = 0; !err && i < nr; i++) {
			table[i] = READ_ONCE(addrs[i]);
			if (!PAGE_ALIGNED(table[i]) ||
			    !vduse_prot_outside((void *)table[i],
						BIT_ULL(shift)))
				err = -EINVAL;
		}
	}

	vduse_prot_mutex_lock(&p->map_lock);
	vduse_guest_free_umem_table(p);
	if (nr && !err) {
		p->umem_addr = table;
		p->umem_nr = nr;
		p->umem_shift = shift;
		p->umem_size = size;
		p->umem_registered = true;
		table = NULL;
	}
	WRITE_ONCE(p->umem_wanted, false);
	vduse_prot_mutex_unlock(&p->map_lock);

	vduse_prot_free(table);
	return err;
}

static void vduse_guest_free_umem(struct vduse_guest_dev *dev)
{
	unsigned int i, order = dev->umem_shift - PAGE_SHIFT;

	vduse_prot_call(vduse_guest_p_drop_umem, dev);
	dev->umem_registered = false;
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

/* Give the protected translation the registered chunks. */
static int vduse_guest_install_umem(struct vduse_guest_dev *dev)
{
	unsigned long *addrs;
	unsigned int i;
	long err;

	addrs = kmalloc_objs(*addrs, dev->umem_nr_chunks);
	if (!addrs)
		return -ENOMEM;
	for (i = 0; i < dev->umem_nr_chunks; i++)
		addrs[i] = (unsigned long)page_address(dev->umem_chunks[i]);
	err = vduse_prot_call(vduse_guest_p_umem_done, dev, addrs,
			      dev->umem_nr_chunks, dev->umem_shift,
			      dev->umem_size);
	kfree(addrs);
	return err;
}

/*
 * Back the bounce region with guest pages. VDUSE accepts this only after
 * the host's first streaming map, so a start asks for it, and the first
 * translation after a start tries until VDUSE reports the region. The host
 * makes its streaming maps before it publishes a chain that uses them, so a
 * chain that waited for the registration only uses the region once it has
 * succeeded or failed. A registration may be left from an earlier start, or
 * from a domain that the host has since replaced, so it is dropped first.
 * Any other failure leaves the region in host memory, and the backends copy.
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

	vduse_prot_call(vduse_guest_p_drop_umem, dev);
	if (!err && size != dev->umem_size)
		err = vduse_guest_alloc_umem(dev, size);
	if (!err) {
		dev->hops->dereg_umem(dev, size);
		err = dev->hops->reg_umem(dev, size);
	}
	if (!err)
		err = vduse_guest_install_umem(dev);
	if (!err) {
		dev->umem_registered = true;
		atomic64_inc(&dev->stats.umem_regs);
	} else {
		vduse_prot_call(vduse_guest_p_umem_done, dev, NULL, 0, 0, 0);
		pr_warn("%s: the bounce region stays in host memory, so requests are copied: %d\n",
			dev->name, err);
	}
	WRITE_ONCE(dev->umem_wanted, false);
}

/* Report the maps that failed since the counter was at errors. */
static void vduse_guest_warn_map(struct vduse_guest_dev *dev, s64 errors)
{
	if (atomic64_read(&dev->stats.map_errors) != errors)
		pr_warn_ratelimited("%s: cannot map IOVA 0x%llx: %d\n",
				    dev->name,
				    READ_ONCE(dev->stats.map_error_iova),
				    READ_ONCE(dev->stats.map_error));
}

int vduse_guest_read(struct vduse_guest_dev *dev, u64 addr, void *buf,
		     size_t len)
{
	s64 errors = atomic64_read(&dev->stats.map_errors);
	long err;

	err = vduse_prot_call(vduse_guest_p_read, dev, addr, buf, len, false);
	if (err == -EAGAIN) {
		vduse_guest_register_umem(dev);
		err = vduse_prot_call(vduse_guest_p_read, dev, addr, buf, len,
				      true);
	}
	vduse_guest_warn_map(dev, errors);
	return err;
}

static void vduse_guest_unmap_range(struct vduse_guest_prot *p, u64 start,
				    u64 last, bool rings)
{
	struct vduse_guest_mapping *m, *n;

	vduse_prot_mutex_lock(&p->map_lock);
	list_for_each_entry_safe(m, n, &p->regions, list) {
		if (m->region.last < start || m->region.start > last)
			continue;
		if (m->region.protect && !rings)
			continue;
		vduse_guest_unmap_region(p, m);
	}
	vduse_prot_mutex_unlock(&p->map_lock);
}

/*
 * Unmap the regions within a range while the device is stopped. Protected
 * rings stay mapped until the queues stop for good, at a reset or the
 * release.
 */
static long __vduse_prot vduse_guest_p_unmap_range(struct vduse_guest_dev *dev,
						   u64 start, u64 last)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	if (!p->running)
		vduse_guest_unmap_range(p, start, last, false);
	vduse_prot_mutex_unlock(&p->life_lock);
	return 0;
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

static u16 vduse_guest_used_idx(struct vduse_guest_pvq *pvq)
{
	return vringh16_to_cpu(&pvq->vrh,
			       READ_ONCE(pvq->vrh.vring.used->idx));
}

/*
 * Record in the log that a request was taken. The head can be taken again as
 * soon as its last completion is published, which is before that completion
 * clears its flag, so the lock orders the two.
 */
static void vduse_guest_log_take(struct vduse_guest_pvq *pvq, u16 head)
{
	struct vduse_guest_log_desc *d = &pvq->log->desc[head];
	unsigned long flags;

	vduse_prot_lock(&pvq->used_lock, flags);
	WRITE_ONCE(d->counter, pvq->counter++);
	smp_wmb();
	WRITE_ONCE(d->inflight, 1);
	vduse_prot_unlock(&pvq->used_lock, flags);
}

static void vduse_guest_free_replay(struct vduse_guest_pvq *pvq)
{
	vduse_prot_free(pvq->replay);
	pvq->replay = NULL;
	pvq->nr_replay = 0;
	pvq->replay_pos = 0;
}

/*
 * Publish completions with one update of the used index, so that the driver
 * sees all of them or none. Each element is read once. A completion is
 * dropped if no backend holds its head or its used length exceeds the
 * writable length of the chain. Returns the number published.
 */
static unsigned int vduse_guest_publish(struct vduse_guest_pvq *pvq,
					const struct vring_used_elem *used,
					unsigned int n)
{
	struct vduse_guest_vq_stats *stats = &pvq->vq->stats;
	struct vring_used_elem e;
	unsigned int i, k = 0;
	unsigned long flags;
	u64 bytes = 0;
	u32 head, len;

	vduse_prot_lock(&pvq->used_lock, flags);
	if (!pvq->started || n > pvq->vrh.vring.num)
		goto out;

	for (i = 0; i < n; i++) {
		e.id = READ_ONCE(used[i].id);
		e.len = READ_ONCE(used[i].len);
		head = vringh32_to_cpu(&pvq->vrh, e.id);
		len = vringh32_to_cpu(&pvq->vrh, e.len);
		if (head >= pvq->vrh.vring.num ||
		    pvq->taken[head] == VDUSE_GUEST_NOT_TAKEN ||
		    len > pvq->taken[head]) {
			atomic64_inc(&stats->bad_completions);
			continue;
		}
		pvq->taken[head] = VDUSE_GUEST_NOT_TAKEN;
		pvq->used[k++] = e;
		bytes += len;
	}
	if (!k)
		goto out;

	/*
	 * The log's used index trails the ring's only between publishing the
	 * completions and clearing their flags, and then the used ring holds
	 * the heads whose flags may still be set.
	 */
	vringh_complete_multi_kern(&pvq->vrh, pvq->used, k);
	for (i = 0; i < k; i++) {
		head = vringh32_to_cpu(&pvq->vrh, pvq->used[i].id);
		WRITE_ONCE(pvq->log->desc[head].inflight, 0);
	}
	smp_wmb();
	WRITE_ONCE(pvq->log->used_idx,
		   pvq->vrh.last_used_idx + pvq->vrh.completed);
out:
	vduse_prot_unlock(&pvq->used_lock, flags);
	atomic64_add(bytes, &stats->in_bytes);
	return k;
}

static int vduse_guest_walk(struct vduse_guest_pvq *pvq, u16 head, bool r,
			    bool w)
{
	return vringh_chain_kern_range(&pvq->vrh, head, r ? &pvq->riov : NULL,
				       w ? &pvq->wiov : NULL,
				       vduse_guest_getrange, 0);
}

/*
 * Copy a chain's buffers to a backend's iov. The backend's kiov is read
 * once, and its array must be large enough and outside protected memory.
 */
static int vduse_guest_copy_kiov(const struct vringh_kiov *from,
				 struct vringh_kiov *to)
{
	struct vringh_kiov k;

	if (!to)
		return 0;
	memcpy(&k, to, sizeof(k));
	if (from->used > (k.max_num & ~VRINGH_IOV_ALLOCATED))
		return -ENOBUFS;
	if (!vduse_prot_outside(k.iov, from->used * sizeof(*k.iov)))
		return -EFAULT;
	memcpy(k.iov, from->iov, from->used * sizeof(*k.iov));
	WRITE_ONCE(to->i, 0);
	WRITE_ONCE(to->used, from->used);
	WRITE_ONCE(to->consumed, 0);
	return 0;
}

static u32 vduse_guest_kiov_length(const struct vringh_kiov *iov)
{
	u64 len = 0;
	unsigned int i;

	for (i = 0; i < iov->used; i++)
		len += iov->iov[i].iov_len;
	return min_t(u64, len, VDUSE_GUEST_NOT_TAKEN - 1);
}

/* Return the pending chain to the driver as used with no data. */
static void vduse_guest_drop(struct vduse_guest_pvq *pvq)
{
	u16 h = pvq->pending_head;
	struct vring_used_elem used = {
		.id = cpu_to_vringh32(&pvq->vrh, h),
	};

	pvq->pending = false;
	atomic64_inc(&pvq->vq->stats.bad_requests);
	pvq->taken[h] = 0;
	vduse_guest_publish(pvq, &used, 1);
}

/*
 * Take the next chain, with requests that an earlier guest left outstanding
 * first, and hand it to the backend. A chain that cannot be walked, or whose
 * buffers the backend's iovs cannot take, is returned to the driver as used
 * with no data. Returns 1 for a chain, 0 if there is none or the queue is
 * not running, -EAGAIN if the bounce region waits for guest pages, -ENOBUFS
 * if the backend's iovs are too small for the chain, or another error if the
 * ring itself is invalid. The chain stays pending for the next call on
 * -EAGAIN and -ENOBUFS.
 */
static long vduse_guest_take(struct vduse_guest_pvq *pvq,
			     struct vringh_kiov *riov,
			     struct vringh_kiov *wiov, u16 *head,
			     bool skip_umem)
{
	struct vduse_guest_vq_stats *stats = &pvq->vq->stats;
	u16 h;
	int err;

	if (!pvq->running)
		return 0;

	for (;;) {
		pvq->need_umem = false;
		pvq->skip_umem = skip_umem;
		if (!pvq->pending || !pvq->walked)
			pvq->riov.used = pvq->wiov.used = 0;

		if (pvq->pending) {
			h = pvq->pending_head;
			err = pvq->walked ? 0 : vduse_guest_walk(pvq, h, riov,
								 wiov);
		} else if (pvq->nr_replay) {
			h = pvq->replay[pvq->replay_pos++];
			if (pvq->replay_pos == pvq->nr_replay)
				vduse_guest_free_replay(pvq);
			pvq->replayed = true;
			err = vduse_guest_walk(pvq, h, riov, wiov);
		} else {
			struct vringh_kiov *r = riov ? &pvq->riov : NULL;
			struct vringh_kiov *w = wiov ? &pvq->wiov : NULL;

			pvq->replayed = false;
			err = vringh_getdesc_kern_range(&pvq->vrh, r, w,
							vduse_guest_getrange,
							&h, 0);
			if (!err)
				return 0;
			if (err < 0 && h == pvq->vrh.vring.num)
				return err;
			vduse_guest_log_take(pvq, h);
			err = min(err, 0);
		}

		if (err && pvq->need_umem) {
			pvq->pending = true;
			pvq->walked = false;
			pvq->pending_head = h;
			return -EAGAIN;
		}
		if (err) {
			pvq->pending_head = h;
			vduse_guest_drop(pvq);
			continue;
		}

		pvq->pending = true;
		pvq->walked = true;
		pvq->pending_head = h;

		err = vduse_guest_copy_kiov(&pvq->riov, riov);
		if (!err)
			err = vduse_guest_copy_kiov(&pvq->wiov, wiov);
		if (err == -ENOBUFS)
			return err;
		if (err) {
			vduse_guest_drop(pvq);
			continue;
		}

		/* Only a head that a backend holds can be completed. */
		pvq->taken[h] = wiov ? vduse_guest_kiov_length(&pvq->wiov) : 0;
		atomic64_inc(&stats->requests);
		if (pvq->replayed)
			atomic64_inc(&stats->replayed);
		if (riov)
			atomic64_add(vduse_guest_kiov_length(&pvq->riov),
				     &stats->out_bytes);
		WRITE_ONCE(*head, h);
		pvq->pending = false;
		return 1;
	}
}

/* Return a pending chain whose buffers the backend cannot take. */
static long __vduse_prot vduse_guest_p_drop(struct vduse_guest_dev *dev,
					    unsigned int index)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);

	if (!pvq)
		return -EINVAL;
	vduse_prot_mutex_lock(&pvq->avail_lock);
	if (pvq->pending && pvq->walked)
		vduse_guest_drop(pvq);
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	return 0;
}

static long __vduse_prot vduse_guest_p_getdesc(struct vduse_guest_dev *dev,
					       unsigned int index,
					       struct vringh_kiov *riov,
					       struct vringh_kiov *wiov,
					       u16 *head, bool skip_umem)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	long r;

	if (!pvq)
		return -EINVAL;
	if ((!riov && !wiov) ||
	    (riov && !vduse_prot_outside(riov, sizeof(*riov))) ||
	    (wiov && !vduse_prot_outside(wiov, sizeof(*wiov))) ||
	    !vduse_prot_outside(head, sizeof(*head)))
		return -EFAULT;

	vduse_prot_mutex_lock(&pvq->avail_lock);
	r = vduse_guest_take(pvq, riov, wiov, head, skip_umem);
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	return r;
}

/*
 * Make room in a backend's iov for a longer chain, as vringh would. Returns 1
 * if it grew, 0 if it is as large as a chain can be, or an error.
 */
static int vduse_guest_grow_kiov(struct vringh_kiov *iov)
{
	unsigned int max = iov->max_num & ~VRINGH_IOV_ALLOCATED, n;
	struct kvec *new;

	if (max >= VDUSE_GUEST_IOV_MAX)
		return 0;
	n = clamp(2 * max, 8U, (unsigned int)VDUSE_GUEST_IOV_MAX);
	if (iov->max_num & VRINGH_IOV_ALLOCATED) {
		new = krealloc_array(iov->iov, n, sizeof(*new), GFP_KERNEL);
	} else {
		new = kmalloc_objs(*new, n);
		if (new)
			memcpy(new, iov->iov, iov->used * sizeof(*new));
	}
	if (!new)
		return -ENOMEM;
	iov->iov = new;
	iov->max_num = n | VRINGH_IOV_ALLOCATED;
	return 1;
}

/*
 * Get the next available descriptor chain with its buffers translated to
 * pointers. Requests that an earlier guest left outstanding come first. A
 * chain that cannot be translated, or that the iovs cannot be grown for, is
 * returned to the driver as used with no data. Returns 1 for a chain, 0 if
 * there is none or the queue is not running, or an error if the ring itself
 * is invalid.
 */
int vduse_guest_getdesc(struct vduse_guest_dev *dev, unsigned int index,
			struct vringh_kiov *riov, struct vringh_kiov *wiov,
			u16 *head)
{
	struct vduse_guest_vq_stats *stats;
	bool skip_umem = false;
	s64 bad, map_errors;
	int grew;
	long r;

	if (index >= dev->num_queues)
		return -EINVAL;
	stats = &dev->vqs[index].stats;
	bad = atomic64_read(&stats->bad_requests);
	map_errors = atomic64_read(&dev->stats.map_errors);

	for (;;) {
		r = vduse_prot_call(vduse_guest_p_getdesc, dev, index, riov,
				    wiov, head, skip_umem);
		if (r == -EAGAIN && !skip_umem) {
			vduse_guest_register_umem(dev);
			skip_umem = true;
			continue;
		}
		if (r != -ENOBUFS)
			break;
		grew = (riov ? vduse_guest_grow_kiov(riov) : 0) |
		       (wiov ? vduse_guest_grow_kiov(wiov) : 0);
		if (grew <= 0)
			vduse_prot_call(vduse_guest_p_drop, dev, index);
	}

	vduse_guest_warn_map(dev, map_errors);
	if (atomic64_read(&stats->bad_requests) != bad) {
		pr_warn_ratelimited("%s: dropped a bad request on queue %u\n",
				    dev->name, index);
		vduse_guest_notify_used(dev, index);
	}
	if (r < 0)
		pr_err_ratelimited("%s: cannot take a request from queue %u: %ld\n",
				   dev->name, index, r);
	return r;
}

/* Fill a used element of the queue in the ring's byte order. */
void vduse_guest_used_elem(struct vduse_guest_dev *dev, unsigned int index,
			   struct vring_used_elem *elem, u16 head, u32 len)
{
	elem->id = __cpu_to_virtio32(dev->ring_le, head);
	elem->len = __cpu_to_virtio32(dev->ring_le, len);
}

static long __vduse_prot
vduse_guest_p_publish(struct vduse_guest_dev *dev, unsigned int index,
		      const struct vring_used_elem *used, unsigned int n)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);

	if (!pvq)
		return -EINVAL;
	if (n > pvq->size || !vduse_prot_outside(used, n * sizeof(*used)))
		return -EFAULT;
	return vduse_guest_publish(pvq, used, n) == n ? 0 : -EINVAL;
}

/*
 * Publish the completions in used, filled by vduse_guest_used_elem(), with
 * one update of the used index, so that the driver sees all of them or none.
 * The driver is not notified until vduse_guest_notify_used(), which covers
 * every completion published since the last notification.
 */
void vduse_guest_publish_n(struct vduse_guest_dev *dev, unsigned int index,
			   const struct vring_used_elem *used, unsigned int n)
{
	if (vduse_prot_call(vduse_guest_p_publish, dev, index, used, n))
		pr_warn_ratelimited("%s: dropped a bad completion on queue %u\n",
				    dev->name, index);
}

static long __vduse_prot vduse_guest_p_need_notify(struct vduse_guest_dev *dev,
						   unsigned int index)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	unsigned long flags;
	int notify = 0;

	if (!pvq)
		return -EINVAL;
	vduse_prot_lock(&pvq->used_lock, flags);
	if (pvq->started && pvq->vrh.completed)
		notify = vringh_need_notify_kern(&pvq->vrh);
	vduse_prot_unlock(&pvq->used_lock, flags);
	return notify;
}

/*
 * Notify the driver of the completions published since the last
 * notification, if it asked to be. Without any, nothing is sent.
 */
void vduse_guest_notify_used(struct vduse_guest_dev *dev, unsigned int index)
{
	if (vduse_prot_call(vduse_guest_p_need_notify, dev, index) <= 0)
		atomic64_inc(&dev->vqs[index].stats.suppressed);
	else
		vduse_guest_inject(dev, index);
}

/* Publish completions and notify the driver of them. */
void vduse_guest_complete_n(struct vduse_guest_dev *dev, unsigned int index,
			    const struct vring_used_elem *used, unsigned int n)
{
	vduse_guest_publish_n(dev, index, used, n);
	vduse_guest_notify_used(dev, index);
}

void vduse_guest_complete(struct vduse_guest_dev *dev, unsigned int index,
			  u16 head, u32 len)
{
	struct vring_used_elem used;

	vduse_guest_used_elem(dev, index, &used, head, len);
	vduse_guest_complete_n(dev, index, &used, 1);
}

/*
 * Enable or disable notifications. Enabling returns 0 if more buffers became
 * available in the meantime, and 1 otherwise. While the queue is stopped, no
 * buffer counts as available, as getdesc takes none.
 */
static long __vduse_prot vduse_guest_p_notify(struct vduse_guest_dev *dev,
					      unsigned int index, bool enable)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	unsigned long flags;
	long r = 1;

	if (!pvq)
		return -EINVAL;
	vduse_prot_lock(&pvq->used_lock, flags);
	if (pvq->started && enable)
		r = vringh_notify_enable_kern(&pvq->vrh) ||
		    !READ_ONCE(pvq->running);
	else if (pvq->started)
		vringh_notify_disable_kern(&pvq->vrh);
	vduse_prot_unlock(&pvq->used_lock, flags);
	return r;
}

void vduse_guest_notify_disable(struct vduse_guest_dev *dev,
				unsigned int index)
{
	vduse_prot_call(vduse_guest_p_notify, dev, index, false);
}

/*
 * Enable notifications, and return false if more buffers became available
 * in the meantime.
 */
bool vduse_guest_notify_enable(struct vduse_guest_dev *dev,
			       unsigned int index)
{
	return vduse_prot_call(vduse_guest_p_notify, dev, index, true) != 0;
}

int vduse_guest_shm_map(struct vduse_guest_dev *dev, u8 id,
			const struct vduse_guest_shm_range *ranges,
			unsigned int nr)
{
	return dev->hops->shm_map(dev, id, ranges, nr);
}

int vduse_guest_shm_unmap(struct vduse_guest_dev *dev, u8 id, u64 offset,
			  u64 size)
{
	return dev->hops->shm_unmap(dev, id, offset, size);
}

int vduse_guest_shm_reg_bar(struct vduse_guest_dev *dev, struct pci_dev *pdev,
			    int bar, u32 *file)
{
	if (!dev->hops->shm_reg_bar)
		return -EOPNOTSUPP;
	return dev->hops->shm_reg_bar(dev, pdev, bar, file);
}

/*
 * Return 1 if the driver has made buffers available that are not taken and
 * the queue is running.
 */
static long __vduse_prot vduse_guest_p_avail(struct vduse_guest_dev *dev,
					     unsigned int index)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	struct vringh *vrh;
	long r = 0;

	if (!pvq)
		return -EINVAL;
	vrh = &pvq->vrh;
	vduse_prot_mutex_lock(&pvq->avail_lock);
	if (pvq->running)
		r = vringh16_to_cpu(vrh, READ_ONCE(vrh->vring.avail->idx)) !=
		    vrh->last_avail_idx;
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	return r;
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
	u64 end;

	if (!dev->poll_ns)
		return false;

	end = ktime_get_ns() + dev->poll_ns;
	do {
		if (vduse_prot_call(vduse_guest_p_avail, dev, index) > 0) {
			atomic64_inc(&vq->stats.polled);
			return true;
		}
		cpu_relax();
	} while (!need_resched() && ktime_get_ns() < end);

	atomic64_inc(&vq->stats.poll_timeouts);
	return false;
}

static size_t vduse_guest_log_queue_size(u16 size)
{
	return struct_size_t(struct vduse_guest_log_queue, desc, size);
}

static void vduse_guest_clear_log(struct vduse_guest_prot *p)
{
	unsigned int i;

	for (i = 0; i < p->num_queues; i++) {
		struct vduse_guest_pvq *pvq = &p->vqs[i];

		/* The size stays, so that a crash here leaves a valid log. */
		pvq->log->used_idx = 0;
		memset(pvq->log->desc, 0, pvq->size * sizeof(*pvq->log->desc));
		pvq->counter = 0;
		vduse_guest_free_replay(pvq);
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
static int vduse_guest_recover_vq(struct vduse_guest_pvq *pvq)
{
	struct vduse_guest_log_queue *log = pvq->log;
	unsigned int num = pvq->vrh.vring.num;
	struct vduse_guest_inflight *in;
	unsigned int i, n = 0;
	u16 used, idx;
	u32 head;

	vduse_guest_free_replay(pvq);
	pvq->counter = 0;

	used = vduse_guest_used_idx(pvq);
	pvq->vrh.last_used_idx = used;

	/*
	 * The earlier guest stopped between publishing completions and
	 * logging them, and the used ring holds their heads.
	 */
	for (idx = log->used_idx; idx != used && (u16)(idx - log->used_idx) < num;
	     idx++) {
		head = READ_ONCE(pvq->vrh.vring.used->ring[idx % num].id);
		head = vringh32_to_cpu(&pvq->vrh, head);
		if (head < pvq->size)
			log->desc[head].inflight = 0;
	}
	log->used_idx = used;

	in = vduse_prot_alloc_array(num, sizeof(*in));
	if (!in)
		return -ENOMEM;

	for (i = 0; i < pvq->size; i++) {
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
		pvq->replay = vduse_prot_alloc_array(n, sizeof(*pvq->replay));
		if (!pvq->replay) {
			vduse_prot_free(in);
			return -ENOMEM;
		}
		for (i = 0; i < n; i++)
			pvq->replay[i] = in[i].head;
		pvq->nr_replay = n;
		pvq->counter = in[n - 1].counter + 1;
	}
	pvq->vrh.last_avail_idx = used + n;
	vduse_prot_free(in);

	WRITE_ONCE(pvq->vq->replay, n);
	return 0;
}

/*
 * Start a queue with the rings that the host gives. No backend holds a head
 * of it until it takes one.
 */
static int vduse_guest_start_vq(struct vduse_guest_prot *p,
				struct vduse_guest_pvq *pvq)
{
	struct vduse_guest_vq_info info;
	void *desc, *avail, *used;
	unsigned long flags;
	unsigned int i;
	size_t num;
	int err;

	err = p->hops->vq_get_info(p->dev, pvq->index, &info);
	if (err)
		return err;

	vduse_prot_mutex_lock(&pvq->avail_lock);
	vduse_prot_lock(&pvq->used_lock, flags);
	pvq->started = false;
	pvq->pending = false;
	vduse_prot_unlock(&pvq->used_lock, flags);

	pvq->ready = info.ready;
	if (!pvq->ready)
		goto out;

	/* The queue's area of the log has an entry for each possible head. */
	num = info.num;
	if (!num || num > pvq->size) {
		err = -EINVAL;
		goto out;
	}
	desc = vduse_guest_translate(p, info.desc_addr,
				     num * sizeof(struct vring_desc), false);
	avail = vduse_guest_translate(p, info.driver_addr,
				      sizeof(struct vring_avail) +
				      (num + 1) * sizeof(__virtio16), false);
	used = vduse_guest_translate(p, info.device_addr,
				     sizeof(struct vring_used) +
				     num * sizeof(struct vring_used_elem) +
				     sizeof(__virtio16), true);
	if (!desc || !avail || !used) {
		err = -EFAULT;
		goto out;
	}

	err = vringh_init_kern(&pvq->vrh, p->features, num, true, desc,
			       avail, used);
	if (err)
		goto out;
	pvq->vrh.last_avail_idx = info.avail_index;
	pvq->vrh.last_used_idx = info.avail_index;

	/* The log's used index follows the ring's from the start. */
	if (p->reconnect) {
		err = vduse_guest_recover_vq(pvq);
		if (err)
			goto out;
	} else {
		WRITE_ONCE(pvq->log->used_idx, vduse_guest_used_idx(pvq));
	}

	vduse_prot_lock(&pvq->used_lock, flags);
	for (i = 0; i < pvq->size; i++)
		pvq->taken[i] = VDUSE_GUEST_NOT_TAKEN;
	pvq->started = true;
	vduse_prot_unlock(&pvq->used_lock, flags);
out:
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	if (err || !pvq->ready)
		return err;
	return p->hops->enable_kick(p->dev, pvq->index);
}

/*
 * Start the queues with the features that the host's driver accepted. No
 * request of this start has been translated yet, so none can point into the
 * host's bounce pages when guest pages replace them. The host's driver
 * resets the device before it starts it again, so a queue that ran since
 * the last reset is not started from the host's saved state.
 */
static long __vduse_prot vduse_guest_p_start(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	unsigned int i;
	int err = 0;

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	for (i = 0; i < p->num_queues; i++)
		if (p->running || p->vqs[i].ran)
			err = -EBUSY;
	if (!err)
		err = p->hops->get_features(dev, &p->features);
	for (i = 0; !err && i < p->num_queues; i++)
		err = vduse_guest_start_vq(p, &p->vqs[i]);
	if (!err) {
		vduse_prot_mutex_lock(&p->map_lock);
		vduse_guest_free_umem_table(p);
		WRITE_ONCE(p->umem_wanted, p->bounce_guest);
		vduse_prot_mutex_unlock(&p->map_lock);
		WRITE_ONCE(dev->features, p->features);
		WRITE_ONCE(dev->ring_le, virtio_legacy_is_little_endian() ||
			   (p->features & BIT_ULL(VIRTIO_F_VERSION_1)));
	}
	vduse_prot_mutex_unlock(&p->life_lock);
	return err;
}

/*
 * Let the backend take chains from the ready queues, or stop it. A start
 * takes over the queues of a reconnecting device.
 */
static long __vduse_prot vduse_guest_p_set_running(struct vduse_guest_dev *dev,
						   bool running)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	struct vduse_guest_pvq *pvq;
	unsigned int i;

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	for (i = 0; i < p->num_queues; i++) {
		pvq = &p->vqs[i];
		vduse_prot_mutex_lock(&pvq->avail_lock);
		pvq->running = running && pvq->started && pvq->ready;
		pvq->ran |= pvq->running;
		WRITE_ONCE(pvq->vq->running, pvq->running);
		vduse_prot_mutex_unlock(&pvq->avail_lock);
	}
	p->running = running;
	if (running)
		p->reconnect = false;
	vduse_prot_mutex_unlock(&p->life_lock);
	return 0;
}

static long __vduse_prot
vduse_guest_p_set_reconnect(struct vduse_guest_dev *dev, bool reconnect)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	p->reconnect = reconnect;
	vduse_prot_mutex_unlock(&p->life_lock);
	return 0;
}

/* Stop the queues for good and unmap their rings. */
static void vduse_guest_end_queues(struct vduse_guest_prot *p)
{
	struct vduse_guest_pvq *pvq;
	unsigned long flags;
	unsigned int i;

	p->running = false;
	for (i = 0; i < p->num_queues; i++) {
		pvq = &p->vqs[i];
		vduse_prot_mutex_lock(&pvq->avail_lock);
		vduse_prot_lock(&pvq->used_lock, flags);
		pvq->started = false;
		pvq->running = false;
		pvq->ran = false;
		pvq->pending = false;
		vduse_prot_unlock(&pvq->used_lock, flags);
		WRITE_ONCE(pvq->vq->running, false);
		vduse_prot_mutex_unlock(&pvq->avail_lock);
	}
	vduse_guest_unmap_range(p, 0, U64_MAX, true);
}

/*
 * The host frees and reallocates the rings and the bounce region without a
 * message, so a reset drops every mapping.
 */
static long __vduse_prot vduse_guest_p_reset(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	vduse_guest_end_queues(p);
	vduse_guest_clear_log(p);
	p->reconnect = false;
	vduse_prot_mutex_unlock(&p->life_lock);
	return 0;
}

/* Unmap everything for the release, and keep the log for the next guest. */
static long __vduse_prot vduse_guest_p_release(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_prot_mutex_lock(&p->life_lock);
	vduse_guest_end_queues(p);
	vduse_prot_mutex_unlock(&p->life_lock);
	return 0;
}

static long __vduse_prot vduse_guest_p_vq_state(struct vduse_guest_dev *dev,
						unsigned int index)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	long r;

	if (!pvq)
		return -EINVAL;
	vduse_prot_mutex_lock(&pvq->avail_lock);
	r = pvq->vrh.last_avail_idx;
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	return r;
}

static int vduse_guest_start(struct vduse_guest_dev *dev)
{
	s64 errors = atomic64_read(&dev->stats.map_errors);
	unsigned int i;
	int err;

	err = vduse_prot_call(vduse_guest_p_start, dev);
	vduse_guest_warn_map(dev, errors);
	if (err)
		return err;
	if (dev->reconnect) {
		for (i = 0; i < dev->num_queues; i++)
			pr_info("%s: queue %u has %u requests to replay\n",
				dev->name, i, READ_ONCE(dev->vqs[i].replay));
	}

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
	vduse_prot_call(vduse_guest_p_set_running, dev, true);
	for (i = 0; i < dev->num_queues; i++) {
		if (!READ_ONCE(dev->vqs[i].running))
			continue;
		if (dev->reconnect)
			vduse_guest_inject(dev, i);
		dev->ops->kick(dev->priv, i);
	}
	dev->reconnect = false;

	return 0;
}

static void vduse_guest_stop(struct vduse_guest_dev *dev)
{
	unsigned int i;

	vduse_prot_call(vduse_guest_p_set_running, dev, false);
	for (i = 0; i < dev->num_queues; i++)
		dev->hops->sync_kick(dev, i);
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
		vduse_prot_call(vduse_guest_p_reset, dev);
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
		vduse_prot_call(vduse_guest_p_set_reconnect, dev, false);
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
	long state;
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
			state = vduse_prot_call(vduse_guest_p_vq_state, dev,
						msg.index);
			if (state >= 0)
				avail_index = state;
			else
				ok = false;
			break;
		case VDUSE_GUEST_MSG_SET_STATUS:
			vduse_guest_set_status(dev, msg.status);
			break;
		case VDUSE_GUEST_MSG_UPDATE_IOTLB:
			if (!dev->running)
				vduse_prot_call(vduse_guest_p_unmap_range, dev,
						msg.start, msg.last);
			break;
		case VDUSE_GUEST_MSG_SET_CONFIG:
			if (dev->ops->config_write &&
			    msg.last <= VDUSE_GUEST_CONFIG_WRITE_MAX)
				dev->ops->config_write(dev->priv, msg.start,
						       msg.data, msg.last);
			break;
		case VDUSE_GUEST_MSG_SHM_FAULT:
			/* A backend that takes fault requests answers them. */
			if (dev->ops->shm_fault) {
				atomic_inc(&dev->shm_faults);
				dev->ops->shm_fault(dev->priv, &msg);
				continue;
			}
			ok = false;
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

void vduse_guest_shm_fault_done(struct vduse_guest_dev *dev,
				const struct vduse_guest_msg *msg, bool ok)
{
	int err = dev->hops->reply(dev, msg, ok, 0);

	if (err)
		pr_warn("%s: cannot answer a fault request: %d\n", dev->name,
			err);
	if (atomic_dec_and_test(&dev->shm_faults))
		wake_up(&dev->shm_faults_wq);
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

static void vduse_guest_free_prot(struct vduse_guest_prot *p)
{
	unsigned int i;

	if (p->vqs) {
		for (i = 0; i < p->num_queues; i++) {
			vduse_prot_free(p->vqs[i].taken);
			vduse_prot_free(p->vqs[i].used);
			vduse_prot_free(p->vqs[i].riov.iov);
			vduse_prot_free(p->vqs[i].wiov.iov);
			vduse_prot_free(p->vqs[i].replay);
		}
	}
	vduse_prot_free(p->vqs);
	vduse_prot_free(p->umem_addr);
	vduse_prot_free(p);
}

/*
 * Create the protected state of a device whose queues have the given sizes,
 * each a power of 2. Returns it, or 0 if there is no memory or the device
 * fails the checks.
 */
static long __vduse_prot vduse_guest_p_create(struct vduse_guest_dev *dev,
					      const u16 *sizes,
					      unsigned int num_queues,
					      bool bounce_guest)
{
	const struct vduse_guest_host_ops *hops = READ_ONCE(dev->hops);
	struct vduse_guest_vq *vqs = READ_ONCE(dev->vqs);
	struct vduse_guest_prot *p;
	struct vduse_guest_pvq *pvq;
	struct kvec *rkvec, *wkvec;
	unsigned int i;

	/*
	 * The host's operations are read-only data, and every pointer into the
	 * device that D1 writes through is outside protected memory.
	 */
	if (!num_queues ||
	    !vduse_prot_outside(sizes, num_queues * sizeof(*sizes)) ||
	    !vduse_prot_outside(dev, sizeof(*dev)) ||
	    !vduse_prot_outside(vqs, num_queues * sizeof(*vqs)) ||
	    !is_kernel_rodata((unsigned long)hops))
		return 0;

	p = vduse_prot_alloc(sizeof(*p));
	if (!p)
		return 0;
	p->dev = dev;
	p->hops = hops;
	p->stats = &dev->stats;
	p->bounce_guest = bounce_guest;
	INIT_LIST_HEAD(&p->regions);
	vduse_prot_mutex_init(&p->map_lock);
	vduse_prot_mutex_init(&p->life_lock);

	p->vqs = vduse_prot_alloc_array(num_queues, sizeof(*p->vqs));
	if (!p->vqs)
		goto fail;
	p->num_queues = num_queues;

	for (i = 0; i < num_queues; i++) {
		pvq = &p->vqs[i];
		pvq->p = p;
		pvq->vq = &vqs[i];
		pvq->index = i;
		pvq->size = READ_ONCE(sizes[i]);
		if (!is_power_of_2(pvq->size))
			goto fail;
		vduse_prot_mutex_init(&pvq->avail_lock);
		vduse_prot_lock_init(&pvq->used_lock);

		pvq->taken = vduse_prot_alloc_array(pvq->size,
						    sizeof(*pvq->taken));
		pvq->used = vduse_prot_alloc_array(pvq->size,
						   sizeof(*pvq->used));
		rkvec = vduse_prot_alloc_array(VDUSE_GUEST_IOV_MAX,
					       sizeof(*rkvec));
		wkvec = vduse_prot_alloc_array(VDUSE_GUEST_IOV_MAX,
					       sizeof(*wkvec));
		vringh_kiov_init(&pvq->riov, rkvec, VDUSE_GUEST_IOV_MAX);
		vringh_kiov_init(&pvq->wiov, wkvec, VDUSE_GUEST_IOV_MAX);
		if (!pvq->taken || !pvq->used || !rkvec || !wkvec)
			goto fail;
		memset(pvq->taken, 0xff, pvq->size * sizeof(*pvq->taken));
	}
	return (long)p;

fail:
	vduse_guest_free_prot(p);
	return 0;
}

static long __vduse_prot vduse_guest_p_destroy(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);

	if (!p)
		return -EINVAL;
	vduse_guest_free_prot(p);
	return 0;
}

/*
 * Each queue's area is checked only after the areas before it have matched,
 * since a different size would move every area after it.
 */
static bool vduse_guest_log_matches(struct vduse_guest_prot *p, u64 node_dev,
				    u64 node_ino)
{
	struct vduse_guest_log_header *hdr = p->log;
	unsigned int i;

	if (hdr->magic != VDUSE_GUEST_LOG_MAGIC ||
	    hdr->version != VDUSE_GUEST_LOG_VERSION ||
	    hdr->num_queues != p->num_queues ||
	    hdr->node_dev != node_dev || hdr->node_ino != node_ino)
		return false;

	for (i = 0; i < p->num_queues; i++)
		if (p->vqs[i].log->size != p->vqs[i].size)
			return false;
	return true;
}

/*
 * Take the mapped in-flight log into protected memory, and check or set it
 * up. Returns -EINVAL if it does not describe the device, which leaves it to
 * vduse_guest_p_close_log(), or another error if it could not be taken, which
 * leaves it to the caller.
 */
static long __vduse_prot vduse_guest_p_open_log(struct vduse_guest_dev *dev,
						void *addr, size_t size,
						u64 node_dev, u64 node_ino)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	struct vduse_guest_log_header *hdr;
	size_t want = sizeof(*hdr), offset;
	unsigned int i;
	int err;

	if (!p || p->log)
		return -EBUSY;
	for (i = 0; i < p->num_queues; i++)
		want += vduse_guest_log_queue_size(p->vqs[i].size);
	if (size != want)
		return -EBUSY;

	err = vduse_prot_adopt(&addr, size);
	if (err)
		return err;
	p->log = addr;
	p->log_size = size;
	p->log_protected = vduse_prot_active();

	offset = sizeof(*hdr);
	for (i = 0; i < p->num_queues; i++) {
		p->vqs[i].log = p->log + offset;
		offset += vduse_guest_log_queue_size(p->vqs[i].size);
	}

	if (p->reconnect)
		return vduse_guest_log_matches(p, node_dev, node_ino) ?
			0 : -EINVAL;

	memset(p->log, 0, p->log_size);
	hdr = p->log;
	hdr->magic = VDUSE_GUEST_LOG_MAGIC;
	hdr->version = VDUSE_GUEST_LOG_VERSION;
	hdr->num_queues = p->num_queues;
	hdr->node_dev = node_dev;
	hdr->node_ino = node_ino;
	for (i = 0; i < p->num_queues; i++)
		p->vqs[i].log->size = p->vqs[i].size;
	return 0;
}

/*
 * Give up the log. Returns its address if the caller must unmap it, or 0 if
 * it was in protected memory and is unmapped.
 */
static long __vduse_prot vduse_guest_p_close_log(struct vduse_guest_dev *dev)
{
	struct vduse_guest_prot *p = vduse_guest_prot_of(dev);
	void *addr;
	unsigned int i;

	if (!p)
		return 0;
	for (i = 0; i < p->num_queues; i++)
		p->vqs[i].log = NULL;
	addr = p->log;
	p->log = NULL;
	if (addr && p->log_protected) {
		vduse_prot_unmap(addr, p->log_size);
		addr = NULL;
	}
	return (long)addr;
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
 * Map the in-flight log. A new device starts with an empty log. Reconnecting
 * needs the log of the earlier guest, since without it the requests that
 * guest left outstanding cannot be found.
 */
static int __init vduse_guest_open_log(struct vduse_guest_dev *dev)
{
	u64 node_dev, node_ino;
	unsigned int i;
	size_t size;
	void *addr;
	int err;

	err = dev->hops->node_id(dev, &node_dev, &node_ino);
	if (err)
		return err;

	size = sizeof(struct vduse_guest_log_header);
	for (i = 0; i < dev->num_queues; i++)
		size += vduse_guest_log_queue_size(dev->vqs[i].size);

	err = dev->hops->map_log(dev, size, !dev->reconnect, &addr);
	if (err == -EINVAL) {
		pr_err("%s: the in-flight log does not describe this device\n",
		       dev->name);
		return -EINVAL;
	}
	if (err)
		return err;
	dev->log_size = size;

	err = vduse_prot_call(vduse_guest_p_open_log, dev, addr, size,
			      node_dev, node_ino);
	if (err == -EINVAL)
		pr_err("%s: the in-flight log does not describe this device\n",
		       dev->name);
	else if (err)
		dev->hops->unmap_log(dev, addr, size);
	if (err && err != -EINVAL)
		dev->log_size = 0;
	return err;
}

static void __init vduse_guest_close_log(struct vduse_guest_dev *dev)
{
	void *addr;

	if (!dev->log_size)
		return;
	addr = (void *)vduse_prot_call(vduse_guest_p_close_log, dev);
	dev->hops->unmap_log(dev, addr, dev->log_size);
	dev->log_size = 0;
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

/* Create the protected state of a device in a free slot. */
static int __init vduse_guest_create_prot(struct vduse_guest_dev *dev,
					  const struct vduse_guest_config *cfg)
{
	struct vduse_guest_prot *p;

	if (vduse_guest_nr_prots == ARRAY_SIZE(vduse_guest_prots))
		return -ENOSPC;
	p = (struct vduse_guest_prot *)vduse_prot_call(vduse_guest_p_create,
						       dev, cfg->queue_sizes,
						       cfg->num_queues,
						       dev->bounce_guest);
	if (!p)
		return -EINVAL;
	dev->slot = vduse_guest_nr_prots++;
	vduse_guest_prots[dev->slot] = p;
	return 0;
}

static void __init vduse_guest_destroy_prot(struct vduse_guest_dev *dev)
{
	if (dev->slot >= ARRAY_SIZE(vduse_guest_prots))
		return;
	vduse_prot_call(vduse_guest_p_destroy, dev);
	vduse_guest_prots[dev->slot] = NULL;
	dev->slot = ARRAY_SIZE(vduse_guest_prots);
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
	if (cfg.num_shm && (cfg.allow_reconnect || !dev->hops->shm_setup))
		return -EINVAL;
	for (i = 0; i < cfg.num_queues; i++)
		if (!is_power_of_2(cfg.queue_sizes[i]))
			return -EINVAL;

	dev->num_queues = cfg.num_queues;
	dev->vqs = kzalloc_objs(*dev->vqs, dev->num_queues);
	if (!dev->vqs)
		return -ENOMEM;

	for (i = 0; i < dev->num_queues; i++)
		dev->vqs[i].size = cfg.queue_sizes[i];

	err = vduse_guest_create_prot(dev, &cfg);
	if (err)
		return err;

	err = vduse_guest_create_host(dev, &cfg);
	if (err < 0)
		goto free;
	dev->reconnect = dev->resume = err;
	vduse_prot_call(vduse_guest_p_set_reconnect, dev, dev->reconnect);

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

	for (i = 0; i < cfg.num_shm; i++) {
		err = dev->hops->shm_setup(dev, cfg.shm[i].id, cfg.shm[i].size);
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
free:
	vduse_guest_destroy_prot(dev);
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

	dev->slot = ARRAY_SIZE(vduse_guest_prots);
	mutex_init(&dev->umem_lock);
	INIT_WORK(&dev->msg_work, vduse_guest_msg_work);
	init_waitqueue_head(&dev->shm_faults_wq);

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
	[VDUSE_GUEST_MSG_SHM_FAULT] = "msg_shm_fault",
	[VDUSE_GUEST_MSG_SET_CONFIG] = "msg_set_config",
	[VDUSE_GUEST_MSG_UNKNOWN] = "msg_unknown",
};

static void vduse_guest_show_counter(struct seq_file *m, int indent,
				     const char *name, atomic64_t *v)
{
	seq_printf(m, "%*s%-*s %lld\n", indent, "", 20 - indent, name,
		   atomic64_read(v));
}

/*
 * The state of a running queue: the driver's available index and the
 * device's next one, the used index, and the event indices that each side
 * publishes for the other with EVENT_IDX.
 */
struct vduse_guest_ring_state {
	u16 avail_idx;
	u16 last_avail_idx;
	u16 used_idx;
	u16 avail_event;
	u16 used_event;
	bool event_indices;
};

static long __vduse_prot
vduse_guest_p_ring_state(struct vduse_guest_dev *dev, unsigned int index,
			 struct vduse_guest_ring_state *s)
{
	struct vduse_guest_pvq *pvq = vduse_guest_pvq_of(dev, index);
	struct vringh *vrh;
	long r = -ENODEV;

	if (!pvq || !vduse_prot_outside(s, sizeof(*s)))
		return -EINVAL;
	vrh = &pvq->vrh;
	vduse_prot_mutex_lock(&pvq->avail_lock);
	if (pvq->running) {
		s->avail_idx = READ_ONCE(vrh->vring.avail->idx);
		s->avail_idx = vringh16_to_cpu(vrh, s->avail_idx);
		s->last_avail_idx = vrh->last_avail_idx;
		s->used_idx = vringh16_to_cpu(vrh,
					      READ_ONCE(vrh->vring.used->idx));
		s->event_indices = vrh->event_indices;
		if (vrh->event_indices) {
			s->avail_event = vringh16_to_cpu(vrh,
				READ_ONCE(vring_avail_event(&vrh->vring)));
			s->used_event = vringh16_to_cpu(vrh,
				READ_ONCE(vring_used_event(&vrh->vring)));
		}
		r = 0;
	}
	vduse_prot_mutex_unlock(&pvq->avail_lock);
	return r;
}

/* The state of a running queue, and whether a kick is waiting unread. */
static void vduse_guest_show_ring(struct seq_file *m,
				  struct vduse_guest_dev *dev,
				  unsigned int index)
{
	struct vduse_guest_ring_state s = {};

	if (vduse_prot_call(vduse_guest_p_ring_state, dev, index, &s))
		return;
	if (dev->hops->kick_pending)
		seq_printf(m, "    %-16s %d\n", "kick_pending",
			   dev->hops->kick_pending(dev, index));
	seq_printf(m, "    %-16s %u\n", "avail_idx", s.avail_idx);
	seq_printf(m, "    %-16s %u\n", "last_avail_idx", s.last_avail_idx);
	seq_printf(m, "    %-16s %u\n", "used_idx", s.used_idx);
	if (!s.event_indices)
		return;
	seq_printf(m, "    %-16s %u\n", "avail_event", s.avail_event);
	seq_printf(m, "    %-16s %u\n", "used_event", s.used_event);
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
		vduse_guest_show_counter(m, 4, "bad_completions",
					 &q->bad_completions);
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
		vduse_guest_show_ring(m, dev, i);
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
	wait_event(dev->shm_faults_wq, !atomic_read(&dev->shm_faults));
	vduse_prot_call(vduse_guest_p_release, dev);
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
