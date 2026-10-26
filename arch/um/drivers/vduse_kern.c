// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-uml: " fmt

#include <linux/interrupt.h>
#include <linux/kstrtox.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vduse_guest.h>
#include <irq_kern.h>
#include <init.h>
#include <mem.h>
#include <os.h>

#include "vduse_user.h"

struct uml_vduse_vq {
	struct vduse_guest_dev *dev;
	unsigned int index;
	int kickfd;
	int irq;
};

/*
 * A device's host fds, its in-flight log file, and the host window in which
 * the chunks of its bounce region lie side by side.
 */
struct uml_vduse {
	int fd;
	int irq;
	struct uml_vduse_vq *vqs;

	char *log_path;
	int log_fd;

	void *umem_window;
	u64 umem_size;
};

static int uml_vduse_control = -1;

static struct uml_vduse *uml_vduse_of(struct vduse_guest_dev *dev)
{
	return vduse_guest_host_data(dev);
}

static int uml_vduse_alloc(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u;

	u = kzalloc_obj(*u);
	if (!u)
		return -ENOMEM;

	u->fd = -1;
	u->log_fd = -1;
	vduse_guest_set_host_data(dev, u);
	return 0;
}

static void uml_vduse_free(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	kfree(u->vqs);
	kfree(u);
}

static int uml_vduse_parse_opt(struct vduse_guest_dev *dev, char *opt)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	if (strstarts(opt, "log=")) {
		u->log_path = opt + strlen("log=");
		return 1;
	}
	if (strstarts(opt, "log_fd=")) {
		if (kstrtoint(opt + strlen("log_fd="), 10, &u->log_fd) ||
		    u->log_fd < 0)
			return -EINVAL;
		return 1;
	}
	return 0;
}

static int uml_vduse_check_opts(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	if (u->log_path && u->log_fd >= 0)
		return -EINVAL;
	if (!u->log_path && u->log_fd < 0) {
		pr_err("%s: the device needs an in-flight log, given with log=<host path> or log_fd=<n>\n",
		       vduse_guest_name(dev));
		return -EINVAL;
	}
	return 0;
}

static int uml_vduse_create(struct vduse_guest_dev *dev,
			    const struct vduse_guest_config *cfg, u64 features,
			    u32 vq_align)
{
	if (uml_vduse_control < 0) {
		uml_vduse_control = uml_vduse_user_open_control();
		if (uml_vduse_control < 0)
			return uml_vduse_control;
	}

	return uml_vduse_user_create(uml_vduse_control, vduse_guest_name(dev),
				     cfg->device_id, features, cfg->num_queues,
				     vq_align, cfg->config, cfg->config_size);
}

static int uml_vduse_destroy(struct vduse_guest_dev *dev)
{
	return uml_vduse_user_destroy(uml_vduse_control, vduse_guest_name(dev));
}

static int uml_vduse_open(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);
	unsigned int i, n = vduse_guest_num_queues(dev);

	u->vqs = kzalloc_objs(*u->vqs, n);
	if (!u->vqs)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		u->vqs[i].dev = dev;
		u->vqs[i].index = i;
		u->vqs[i].kickfd = -1;
	}

	u->fd = uml_vduse_user_open(vduse_guest_name(dev));
	if (u->fd < 0)
		return u->fd;

	/* Eventfds raise no SIGIO themselves, so the SIGIO helper watches them. */
	sigio_broken();
	return 0;
}

static void uml_vduse_close(struct vduse_guest_dev *dev)
{
	os_close_file(uml_vduse_of(dev)->fd);
}

static int uml_vduse_node_id(struct vduse_guest_dev *dev, u64 *node_dev,
			     u64 *node_ino)
{
	return uml_vduse_user_node_id(uml_vduse_of(dev)->fd, node_dev,
				      node_ino);
}

static int uml_vduse_request_irq(int fd, irq_handler_t handler, void *opaque)
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

static irqreturn_t uml_vduse_msg_interrupt(int irq, void *opaque)
{
	vduse_guest_msg_event(opaque);
	return IRQ_HANDLED;
}

static int uml_vduse_enable_msgs(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	u->irq = uml_vduse_request_irq(u->fd, uml_vduse_msg_interrupt, dev);
	return u->irq < 0 ? u->irq : 0;
}

static void uml_vduse_disable_msgs(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	uml_vduse_free_irq(u->fd, u->irq, dev);
}

static int uml_vduse_read_msg(struct vduse_guest_dev *dev,
			      struct vduse_guest_msg *msg)
{
	struct uml_vduse_user_msg m;
	int err;

	/* Both number the message types alike. */
	BUILD_BUG_ON((int)UML_VDUSE_MSG_GET_VQ_STATE !=
		     VDUSE_GUEST_MSG_GET_VQ_STATE);
	BUILD_BUG_ON((int)UML_VDUSE_MSG_SET_STATUS != VDUSE_GUEST_MSG_SET_STATUS);
	BUILD_BUG_ON((int)UML_VDUSE_MSG_UPDATE_IOTLB !=
		     VDUSE_GUEST_MSG_UPDATE_IOTLB);
	BUILD_BUG_ON((int)UML_VDUSE_MSG_UNKNOWN != VDUSE_GUEST_MSG_UNKNOWN);

	err = uml_vduse_user_read_msg(uml_vduse_of(dev)->fd, &m);
	if (err)
		return err;

	msg->type = m.type;
	msg->request_id = m.request_id;
	msg->index = m.index;
	msg->status = m.status;
	msg->start = m.start;
	msg->last = m.last;
	return 0;
}

static int uml_vduse_reply(struct vduse_guest_dev *dev,
			   const struct vduse_guest_msg *msg, bool ok,
			   u16 avail_index)
{
	struct uml_vduse_user_msg m = {
		.type = msg->type,
		.request_id = msg->request_id,
		.index = msg->index,
	};

	return uml_vduse_user_reply(uml_vduse_of(dev)->fd, &m, ok, avail_index);
}

static int uml_vduse_get_features(struct vduse_guest_dev *dev, u64 *features)
{
	return uml_vduse_user_get_features(uml_vduse_of(dev)->fd, features);
}

static int uml_vduse_vq_setup(struct vduse_guest_dev *dev, unsigned int index,
			      u16 max_size)
{
	return uml_vduse_user_vq_setup(uml_vduse_of(dev)->fd, index, max_size);
}

static int uml_vduse_vq_get_info(struct vduse_guest_dev *dev,
				 unsigned int index,
				 struct vduse_guest_vq_info *info)
{
	struct uml_vduse_user_vq_info i;
	int err;

	err = uml_vduse_user_vq_get_info(uml_vduse_of(dev)->fd, index, &i);
	if (err)
		return err;

	info->num = i.num;
	info->desc_addr = i.desc_addr;
	info->driver_addr = i.driver_addr;
	info->device_addr = i.device_addr;
	info->avail_index = i.avail_index;
	info->ready = i.ready;
	return 0;
}

static irqreturn_t uml_vduse_kick_interrupt(int irq, void *opaque)
{
	struct uml_vduse_vq *vq = opaque;
	u64 v;
	int r;

	/* A read clears the whole count. */
	do {
		r = os_read_file(vq->kickfd, &v, sizeof(v));
	} while (r == -EINTR);

	vduse_guest_kick(vq->dev, vq->index, r == sizeof(v) ? v : 0);
	return IRQ_HANDLED;
}

static int uml_vduse_open_vq(struct vduse_guest_dev *dev, unsigned int index,
			     unsigned int cpu)
{
	struct uml_vduse_vq *vq = &uml_vduse_of(dev)->vqs[index];
	int err;

	vq->kickfd = uml_vduse_user_create_kickfd();
	if (vq->kickfd < 0)
		return vq->kickfd;

	vq->irq = uml_vduse_request_irq(vq->kickfd, uml_vduse_kick_interrupt,
					vq);
	if (vq->irq < 0) {
		err = vq->irq;
		os_close_file(vq->kickfd);
		return err;
	}

	irq_set_affinity(vq->irq, cpumask_of(cpu));
	return 0;
}

static void uml_vduse_close_vq(struct vduse_guest_dev *dev, unsigned int index)
{
	struct uml_vduse_vq *vq = &uml_vduse_of(dev)->vqs[index];

	uml_vduse_free_irq(vq->kickfd, vq->irq, vq);
	os_close_file(vq->kickfd);
}

static int uml_vduse_enable_kick(struct vduse_guest_dev *dev,
				 unsigned int index)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	return uml_vduse_user_vq_set_kickfd(u->fd, index, u->vqs[index].kickfd);
}

static void uml_vduse_sync_kick(struct vduse_guest_dev *dev,
				unsigned int index)
{
	synchronize_irq(uml_vduse_of(dev)->vqs[index].irq);
}

static int uml_vduse_inject(struct vduse_guest_dev *dev, unsigned int index)
{
	return uml_vduse_user_vq_inject_irq(uml_vduse_of(dev)->fd, index);
}

static int uml_vduse_map(struct vduse_guest_dev *dev, u64 iova,
			 struct vduse_guest_region *region)
{
	struct uml_vduse_user_region r;
	int err;

	err = uml_vduse_user_map(uml_vduse_of(dev)->fd, iova, &r);
	if (err)
		return err;

	region->start = r.start;
	region->last = r.last;
	region->addr = r.addr;
	region->readable = r.readable;
	region->writable = r.writable;
	return 0;
}

static void uml_vduse_unmap(struct vduse_guest_dev *dev,
			    struct vduse_guest_region *region)
{
	struct uml_vduse_user_region r = {
		.start = region->start,
		.last = region->last,
		.addr = region->addr,
	};

	uml_vduse_user_unmap(&r);
}

static int uml_vduse_bounce_size(struct vduse_guest_dev *dev, u64 *size)
{
	return uml_vduse_user_bounce_size(uml_vduse_of(dev)->fd, size);
}

static void uml_vduse_unmap_umem(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	if (u->umem_window)
		uml_vduse_user_unmap_window(u->umem_window, u->umem_size);
	u->umem_window = NULL;
	u->umem_size = 0;
}

/* Map each chunk's range of the memory file side by side into a window. */
static int uml_vduse_map_umem(struct vduse_guest_dev *dev,
			      struct page **chunks, unsigned int nr_chunks,
			      unsigned int chunk_shift, u64 size)
{
	struct uml_vduse *u = uml_vduse_of(dev);
	u64 chunk = BIT_ULL(chunk_shift), off;
	unsigned long long phys;
	unsigned int i;
	int fd, err;

	u->umem_window = uml_vduse_user_reserve_window(size);
	if (!u->umem_window)
		return -ENOMEM;
	u->umem_size = size;

	for (i = 0, off = 0; i < nr_chunks; i++, off += chunk) {
		fd = phys_mapping(page_to_phys(chunks[i]), &phys);
		err = uml_vduse_user_map_window(u->umem_window + off,
						min(chunk, size - off), fd,
						phys);
		if (err) {
			uml_vduse_unmap_umem(dev);
			return err;
		}
	}

	return 0;
}

static int uml_vduse_reg_umem(struct vduse_guest_dev *dev, u64 size)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	return uml_vduse_user_reg_umem(u->fd, u->umem_window, size);
}

static int uml_vduse_dereg_umem(struct vduse_guest_dev *dev, u64 size)
{
	return uml_vduse_user_dereg_umem(uml_vduse_of(dev)->fd, size);
}

/* A log given by path is opened here and closed with its mapping. */
static int uml_vduse_map_log(struct vduse_guest_dev *dev, size_t size,
			     bool resize, void **addr)
{
	struct uml_vduse *u = uml_vduse_of(dev);
	int err;

	if (u->log_path) {
		u->log_fd = uml_vduse_user_open_log(u->log_path);
		if (u->log_fd < 0)
			return u->log_fd;
	}

	err = uml_vduse_user_map_log(u->log_fd, size, resize, addr);
	if (err && u->log_path) {
		os_close_file(u->log_fd);
		u->log_fd = -1;
	}
	return err;
}

static void uml_vduse_unmap_log(struct vduse_guest_dev *dev, void *addr,
				size_t size)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	uml_vduse_user_unmap_log(addr, size);
	if (u->log_path) {
		os_close_file(u->log_fd);
		u->log_fd = -1;
	}
}

static void uml_vduse_drop_log(struct vduse_guest_dev *dev)
{
	struct uml_vduse *u = uml_vduse_of(dev);

	if (u->log_path)
		uml_vduse_user_remove_log(u->log_path);
	else
		uml_vduse_user_truncate_log(u->log_fd);
}

static const struct vduse_guest_host_ops uml_vduse_host_ops = {
	.alloc = uml_vduse_alloc,
	.free = uml_vduse_free,
	.parse_opt = uml_vduse_parse_opt,
	.check_opts = uml_vduse_check_opts,
	.create = uml_vduse_create,
	.destroy = uml_vduse_destroy,
	.open = uml_vduse_open,
	.close = uml_vduse_close,
	.node_id = uml_vduse_node_id,
	.enable_msgs = uml_vduse_enable_msgs,
	.disable_msgs = uml_vduse_disable_msgs,
	.read_msg = uml_vduse_read_msg,
	.reply = uml_vduse_reply,
	.get_features = uml_vduse_get_features,
	.vq_setup = uml_vduse_vq_setup,
	.vq_get_info = uml_vduse_vq_get_info,
	.open_vq = uml_vduse_open_vq,
	.close_vq = uml_vduse_close_vq,
	.enable_kick = uml_vduse_enable_kick,
	.sync_kick = uml_vduse_sync_kick,
	.inject = uml_vduse_inject,
	.map = uml_vduse_map,
	.unmap = uml_vduse_unmap,
	.bounce_size = uml_vduse_bounce_size,
	.map_umem = uml_vduse_map_umem,
	.unmap_umem = uml_vduse_unmap_umem,
	.reg_umem = uml_vduse_reg_umem,
	.dereg_umem = uml_vduse_dereg_umem,
	.map_log = uml_vduse_map_log,
	.unmap_log = uml_vduse_unmap_log,
	.drop_log = uml_vduse_drop_log,
};

static struct vduse_guest_host uml_vduse_host = {
	.ops = &uml_vduse_host_ops,
};

static int __init uml_vduse_init(void)
{
	vduse_guest_register_host(&uml_vduse_host);
	return 0;
}
device_initcall(uml_vduse_init);

__uml_help(uml_vduse_host,
"vduse_guest.device=<type>,<name>[,<options>]\n"
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
