// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-uml: " fmt

#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <uapi/linux/virtio_config.h>
#include <irq_kern.h>
#include <init.h>
#include <os.h>

#include "vduse.h"
#include "vduse_user.h"

#define UML_VDUSE_MAX_DEVICES 8

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
};

struct uml_vduse_dev_stats {
	atomic64_t msgs[UML_VDUSE_MSG_UNKNOWN + 1];
	atomic64_t starts;
	atomic64_t resets;
	atomic64_t maps;
	atomic64_t unmaps;
	atomic64_t map_errors;
};

struct uml_vduse_vq {
	struct uml_vduse_dev *dev;
	unsigned int index;
	int kickfd;
	int irq;
	struct vringh vrh;
	/*
	 * Protects the used ring. The available ring is only accessed from
	 * the backend's processing of the queue.
	 */
	spinlock_t lock;
	bool ready;
	bool running;
	struct uml_vduse_vq_stats stats;
};

struct uml_vduse_dev {
	char *type;
	char *name;
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

	/* Regions of the IOVA space mapped into UML, protected by map_lock. */
	struct list_head regions;
	struct mutex map_lock;

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

static void *uml_vduse_translate(struct uml_vduse_dev *dev, u64 iova,
				 size_t len, bool write)
{
	struct uml_vduse_region *r;
	u64 last;
	int err;

	if (!len || check_add_overflow(iova, (u64)len - 1, &last))
		return NULL;

	guard(mutex)(&dev->map_lock);

	list_for_each_entry(r, &dev->regions, list) {
		if (iova >= r->user.start && iova <= r->user.last)
			goto found;
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

found:
	if (last > r->user.last)
		return NULL;
	if (write ? !r->user.writable : !r->user.readable)
		return NULL;
	return r->user.addr + (iova - r->user.start);
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

static int uml_vduse_translate_iov(struct uml_vduse_dev *dev,
				   struct vringh_kiov *iov, bool write)
{
	unsigned int i;

	if (!iov)
		return 0;

	for (i = 0; i < iov->used; i++) {
		struct kvec *v = &iov->iov[i];
		void *addr = NULL;

		if (v->iov_len) {
			addr = uml_vduse_translate(dev,
						   (unsigned long)v->iov_base,
						   v->iov_len, write);
			if (!addr)
				return -EFAULT;
		}
		v->iov_base = addr;
	}

	return 0;
}

/*
 * Get the next available descriptor chain with its buffers translated to
 * pointers. A chain that cannot be translated is returned to the driver as
 * used with no data. Returns 1 for a chain, 0 if there is none, or an error
 * if the ring itself is invalid.
 */
int uml_vduse_getdesc(struct uml_vduse_dev *dev, unsigned int index,
		      struct vringh_kiov *riov, struct vringh_kiov *wiov,
		      u16 *head)
{
	struct uml_vduse_vq *vq = &dev->vqs[index];
	int err;

	for (;;) {
		*head = vq->vrh.vring.num;
		err = vringh_getdesc_kern(&vq->vrh, riov, wiov, head,
					  GFP_KERNEL);
		if (err > 0) {
			err = uml_vduse_translate_iov(dev, riov, false);
			if (!err)
				err = uml_vduse_translate_iov(dev, wiov, true);
			if (!err) {
				atomic64_inc(&vq->stats.requests);
				if (riov)
					atomic64_add(vringh_kiov_length(riov),
						     &vq->stats.out_bytes);
				return 1;
			}
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

	spin_lock_irqsave(&vq->lock, flags);
	vringh_complete_kern(&vq->vrh, head, len);
	notify = vringh_need_notify_kern(&vq->vrh);
	spin_unlock_irqrestore(&vq->lock, flags);

	atomic64_add(len, &vq->stats.in_bytes);
	if (notify <= 0)
		atomic64_inc(&vq->stats.suppressed);
	else if (uml_vduse_user_vq_inject_irq(dev->fd, index))
		atomic64_inc(&vq->stats.inject_errors);
	else
		atomic64_inc(&vq->stats.injected);
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

	err = dev->ops->start(dev->priv);
	if (err)
		return err;
	dev->running = true;

	/* Kicks that arrived before the queues were running were dropped. */
	for (i = 0; i < dev->num_queues; i++) {
		if (!dev->vqs[i].ready)
			continue;
		WRITE_ONCE(dev->vqs[i].running, true);
		dev->ops->kick(dev->priv, i);
	}

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
		atomic64_inc(&dev->stats.resets);
	}

	return err;
}

static void uml_vduse_msg_work(struct work_struct *work)
{
	struct uml_vduse_dev *dev = container_of(work, struct uml_vduse_dev,
						 msg_work);
	struct uml_vduse_user_msg msg;
	u16 avail_index;
	bool ok;
	int err;

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

static void __init uml_vduse_free_irq(int fd, int irq, void *opaque)
{
	ignore_sigio_fd(fd);
	um_free_irq(irq, opaque);
}

/* Create the host device, removing a leftover one with the same name. */
static int __init uml_vduse_create_host(struct uml_vduse_dev *dev,
					const struct uml_vduse_config *cfg)
{
	u64 features = cfg->features | BIT_ULL(VIRTIO_F_VERSION_1) |
		       BIT_ULL(VIRTIO_F_ACCESS_PLATFORM);
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
	if (err == -EBUSY) {
		pr_err("%s: a device with this name is still bound, run 'vdpa dev del %s' on the host\n",
		       dev->name, dev->name);
		return -EEXIST;
	}
	if (err)
		return err;

	return uml_vduse_user_create(uml_vduse_control, dev->name,
				     cfg->device_id, features,
				     cfg->num_queues, PAGE_SIZE, cfg->config,
				     cfg->config_size);
}

static void __init uml_vduse_close_vqs(struct uml_vduse_dev *dev,
				       unsigned int n)
{
	while (n--) {
		uml_vduse_free_irq(dev->vqs[n].kickfd, dev->vqs[n].irq,
				   &dev->vqs[n]);
		os_close_file(dev->vqs[n].kickfd);
	}
}

static int __init uml_vduse_open_vqs(struct uml_vduse_dev *dev)
{
	unsigned int i;
	int err;

	for (i = 0; i < dev->num_queues; i++) {
		struct uml_vduse_vq *vq = &dev->vqs[i];

		err = uml_vduse_user_vq_setup(dev->fd, i, dev->queue_size);
		if (err)
			goto close;

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
	if (err)
		return err;

	dev->fd = uml_vduse_user_open(dev->name);
	if (dev->fd < 0) {
		err = dev->fd;
		goto destroy;
	}

	err = uml_vduse_open_vqs(dev);
	if (err)
		goto close;

	dev->irq = uml_vduse_request_irq(dev->fd, uml_vduse_msg_interrupt, dev);
	if (dev->irq < 0) {
		err = dev->irq;
		uml_vduse_close_vqs(dev, dev->num_queues);
		goto close;
	}

	/* Messages sent before the IRQ existed raise no event. */
	schedule_work(&dev->msg_work);
	return 0;

close:
	os_close_file(dev->fd);
destroy:
	uml_vduse_user_destroy(uml_vduse_control, dev->name);
	return err;
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
	if (sep) {
		*sep = '\0';
		dev->opts = sep + 1;
	}

	err = uml_vduse_setup(dev);
	if (err) {
		pr_err("%s: cannot create the %s device: %d\n", dev->name,
		       dev->type, err);
		if (dev->ops && !IS_ERR_OR_NULL(dev->priv))
			dev->ops->release(dev->priv);
		goto free;
	}

	pr_info("%s: created %s device\n", dev->name, dev->type);
	list_add_tail(&dev->list, &uml_vduse_devices);
	return;

free:
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
"    <disk> is /dev/<name> or <major>:<minor>.\n\n"
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
	seq_printf(m, "  %-18s 0x%llx\n", "features", READ_ONCE(dev->features));

	for (i = 0; i < ARRAY_SIZE(uml_vduse_msg_names); i++)
		uml_vduse_show_counter(m, 2, uml_vduse_msg_names[i],
				       &stats->msgs[i]);
	uml_vduse_show_counter(m, 2, "starts", &stats->starts);
	uml_vduse_show_counter(m, 2, "resets", &stats->resets);
	uml_vduse_show_counter(m, 2, "iova_maps", &stats->maps);
	uml_vduse_show_counter(m, 2, "iova_unmaps", &stats->unmaps);
	uml_vduse_show_counter(m, 2, "iova_map_errors", &stats->map_errors);

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
	}
}

static int __maybe_unused uml_vduse_proc_show(struct seq_file *m, void *v)
{
	struct uml_vduse_dev *dev;

	list_for_each_entry(dev, &uml_vduse_devices, list)
		uml_vduse_show_device(m, dev);

	return 0;
}

static int __init uml_vduse_init(void)
{
	int i;

	if (!uml_vduse_nr_args)
		return 0;

	sigio_broken();

	for (i = 0; i < uml_vduse_nr_args; i++)
		uml_vduse_add(uml_vduse_args[i]);

	proc_create_single("vduse_uml", 0444, NULL, uml_vduse_proc_show);
	return 0;
}
late_initcall(uml_vduse_init);
