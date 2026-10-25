// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vduse.h>
#include <um_malloc.h>

#include "vduse_user.h"

int uml_vduse_user_open_control(void)
{
	uint64_t version = 0;
	int fd;

	fd = open("/dev/vduse/control", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	if (ioctl(fd, VDUSE_SET_API_VERSION, &version) < 0) {
		int err = -errno;

		close(fd);
		return err;
	}

	return fd;
}

int uml_vduse_user_create(int control, const char *name, uint32_t device_id,
			  uint64_t features, uint32_t vq_num, uint32_t vq_align,
			  const void *config, uint32_t config_size)
{
	struct vduse_dev_config *cfg;
	int err = 0;

	if (strlen(name) >= VDUSE_NAME_MAX)
		return -ENAMETOOLONG;

	cfg = uml_kmalloc(sizeof(*cfg) + config_size, UM_GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	memset(cfg, 0, sizeof(*cfg));
	strcpy(cfg->name, name);
	cfg->device_id = device_id;
	cfg->features = features;
	cfg->vq_num = vq_num;
	cfg->vq_align = vq_align;
	cfg->config_size = config_size;
	memcpy(cfg->config, config, config_size);

	if (ioctl(control, VDUSE_CREATE_DEV, cfg) < 0)
		err = -errno;

	kfree(cfg);
	return err;
}

int uml_vduse_user_destroy(int control, const char *name)
{
	char buf[VDUSE_NAME_MAX] = {};

	if (strlen(name) >= VDUSE_NAME_MAX)
		return -ENAMETOOLONG;
	strcpy(buf, name);

	if (ioctl(control, VDUSE_DESTROY_DEV, buf) < 0)
		return -errno;
	return 0;
}

int uml_vduse_user_open(const char *name)
{
	char path[sizeof("/dev/vduse/") + VDUSE_NAME_MAX];
	int fd;

	snprintf(path, sizeof(path), "/dev/vduse/%s", name);
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	return fd;
}

/* Read one control message, or return -EAGAIN if there is none. */
int uml_vduse_user_read_msg(int fd, struct uml_vduse_user_msg *msg)
{
	struct vduse_dev_request req;
	ssize_t n;

	n = read(fd, &req, sizeof(req));
	if (n < 0)
		return -errno;
	if (n != sizeof(req))
		return -EIO;

	memset(msg, 0, sizeof(*msg));
	msg->request_id = req.request_id;

	switch (req.type) {
	case VDUSE_GET_VQ_STATE:
		msg->type = UML_VDUSE_MSG_GET_VQ_STATE;
		msg->index = req.vq_state.index;
		break;
	case VDUSE_SET_STATUS:
		msg->type = UML_VDUSE_MSG_SET_STATUS;
		msg->status = req.s.status;
		break;
	case VDUSE_UPDATE_IOTLB:
		msg->type = UML_VDUSE_MSG_UPDATE_IOTLB;
		msg->start = req.iova.start;
		msg->last = req.iova.last;
		break;
	default:
		msg->type = UML_VDUSE_MSG_UNKNOWN;
		break;
	}

	return 0;
}

int uml_vduse_user_reply(int fd, const struct uml_vduse_user_msg *msg,
			 int ok, uint16_t avail_index)
{
	struct vduse_dev_response resp;
	ssize_t n;

	memset(&resp, 0, sizeof(resp));
	resp.request_id = msg->request_id;
	resp.result = ok ? VDUSE_REQ_RESULT_OK : VDUSE_REQ_RESULT_FAILED;
	if (msg->type == UML_VDUSE_MSG_GET_VQ_STATE) {
		resp.vq_state.index = msg->index;
		resp.vq_state.split.avail_index = avail_index;
	}

	n = write(fd, &resp, sizeof(resp));
	if (n < 0)
		return -errno;
	return n == sizeof(resp) ? 0 : -EIO;
}

int uml_vduse_user_get_features(int fd, uint64_t *features)
{
	if (ioctl(fd, VDUSE_DEV_GET_FEATURES, features) < 0)
		return -errno;
	return 0;
}

int uml_vduse_user_vq_setup(int fd, uint32_t index, uint16_t max_size)
{
	struct vduse_vq_config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.index = index;
	cfg.max_size = max_size;

	if (ioctl(fd, VDUSE_VQ_SETUP, &cfg) < 0)
		return -errno;
	return 0;
}

int uml_vduse_user_vq_get_info(int fd, uint32_t index,
			       struct uml_vduse_user_vq_info *info)
{
	struct vduse_vq_info vq;

	memset(&vq, 0, sizeof(vq));
	vq.index = index;

	if (ioctl(fd, VDUSE_VQ_GET_INFO, &vq) < 0)
		return -errno;

	info->num = vq.num;
	info->desc_addr = vq.desc_addr;
	info->driver_addr = vq.driver_addr;
	info->device_addr = vq.device_addr;
	info->avail_index = vq.split.avail_index;
	info->ready = vq.ready;
	return 0;
}

int uml_vduse_user_vq_set_kickfd(int fd, uint32_t index, int kickfd)
{
	struct vduse_vq_eventfd efd = {
		.index = index,
		.fd = kickfd,
	};

	if (ioctl(fd, VDUSE_VQ_SETUP_KICKFD, &efd) < 0)
		return -errno;
	return 0;
}

int uml_vduse_user_vq_inject_irq(int fd, uint32_t index)
{
	if (ioctl(fd, VDUSE_VQ_INJECT_IRQ, &index) < 0)
		return -errno;
	return 0;
}

int uml_vduse_user_create_kickfd(void)
{
	int fd;

	fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (fd < 0)
		return -errno;
	return fd;
}

/* Map the whole IOTLB entry that holds an IOVA. */
int uml_vduse_user_map(int fd, uint64_t iova,
		       struct uml_vduse_user_region *region)
{
	struct vduse_iotlb_entry entry;
	int prot = 0, mfd, err = 0;
	uint64_t size;
	void *addr;

	memset(&entry, 0, sizeof(entry));
	entry.start = iova;
	entry.last = iova;

	mfd = ioctl(fd, VDUSE_IOTLB_GET_FD, &entry);
	if (mfd < 0)
		return -errno;

	if (entry.perm & VDUSE_ACCESS_RO)
		prot |= PROT_READ;
	if (entry.perm & VDUSE_ACCESS_WO)
		prot |= PROT_WRITE;

	size = entry.last - entry.start + 1;
	addr = mmap(NULL, size, prot, MAP_SHARED, mfd, entry.offset);
	if (addr == MAP_FAILED) {
		err = -errno;
		goto out;
	}

	region->start = entry.start;
	region->last = entry.last;
	region->addr = addr;
	region->readable = !!(entry.perm & VDUSE_ACCESS_RO);
	region->writable = !!(entry.perm & VDUSE_ACCESS_WO);
out:
	close(mfd);
	return err;
}

void uml_vduse_user_unmap(struct uml_vduse_user_region *region)
{
	munmap(region->addr, region->last - region->start + 1);
}
