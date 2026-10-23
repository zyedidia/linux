// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Ant Group
 * Author: Tiwei Bie <tiwei.btw@antgroup.com>
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <linux/limits.h>
#include <linux/vfio.h>
#include <linux/pci_regs.h>
#include <as-layout.h>
#include <um_malloc.h>

#include "vfio_user.h"

int uml_vfio_user_open_container(void)
{
	int r, fd;

	fd = open("/dev/vfio/vfio", O_RDWR);
	if (fd < 0)
		return -errno;

	r = ioctl(fd, VFIO_GET_API_VERSION);
	if (r != VFIO_API_VERSION) {
		r = r < 0 ? -errno : -EINVAL;
		goto error;
	}

	r = ioctl(fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU);
	if (r <= 0) {
		r = r < 0 ? -errno : -EINVAL;
		goto error;
	}

	return fd;

error:
	close(fd);
	return r;
}

int uml_vfio_user_setup_iommu(int container)
{
	if (ioctl(container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU) < 0)
		return -errno;
	return 0;
}

static int vfio_get_iommu_info(int container,
			       struct vfio_iommu_type1_info **infop)
{
	struct vfio_iommu_type1_info *info;
	uint32_t argsz = sizeof(*info);
	int err;

	for (;;) {
		info = uml_kmalloc(argsz, UM_GFP_KERNEL);
		if (!info)
			return -ENOMEM;

		memset(info, 0, argsz);
		info->argsz = argsz;

		if (ioctl(container, VFIO_IOMMU_GET_INFO, info) < 0) {
			err = -errno;
			kfree(info);
			return err;
		}
		if (info->argsz <= argsz) {
			*infop = info;
			return 0;
		}

		argsz = info->argsz;
		kfree(info);
	}
}

/*
 * Return the number of usable IOVA ranges and store them, sorted by address,
 * in a newly allocated array.
 */
int uml_vfio_user_get_iova_ranges(int container,
				  struct uml_vfio_user_iova_range **ranges)
{
	struct vfio_iommu_type1_info_cap_iova_range *cap = NULL;
	struct vfio_iommu_type1_info *info;
	struct vfio_info_cap_header *hdr;
	uint32_t off;
	int i, n;

	n = vfio_get_iommu_info(container, &info);
	if (n)
		return n;

	if (!(info->flags & VFIO_IOMMU_INFO_PGSIZES) ||
	    !(info->iova_pgsizes & UM_KERN_PAGE_SIZE)) {
		n = -EINVAL;
		goto out;
	}

	if (info->flags & VFIO_IOMMU_INFO_CAPS) {
		for (off = info->cap_offset; off; off = hdr->next) {
			if (off + sizeof(*hdr) > info->argsz)
				break;
			hdr = (void *)info + off;
			if (hdr->id == VFIO_IOMMU_TYPE1_INFO_CAP_IOVA_RANGE) {
				cap = (void *)hdr;
				break;
			}
		}
	}

	n = cap ? cap->nr_iovas : 1;
	*ranges = uml_kmalloc(sizeof(**ranges) * n, UM_GFP_KERNEL);
	if (!*ranges) {
		n = -ENOMEM;
		goto out;
	}

	for (i = 0; i < n; i++) {
		(*ranges)[i].start = cap ? cap->iova_ranges[i].start : 0;
		(*ranges)[i].end = cap ? cap->iova_ranges[i].end : UINT64_MAX;
	}

out:
	kfree(info);
	return n;
}

int uml_vfio_user_dma_map(int container, unsigned long vaddr, uint64_t iova,
			  uint64_t size, int read, int write)
{
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.vaddr = vaddr,
		.iova = iova,
		.size = size,
	};
	int r;

	if (read)
		map.flags |= VFIO_DMA_MAP_FLAG_READ;
	if (write)
		map.flags |= VFIO_DMA_MAP_FLAG_WRITE;

	do {
		r = ioctl(container, VFIO_IOMMU_MAP_DMA, &map);
	} while (r < 0 && errno == EINTR);

	return r < 0 ? -errno : 0;
}

/* Return the number of bytes unmapped. */
int64_t uml_vfio_user_dma_unmap(int container, uint64_t iova, uint64_t size)
{
	struct vfio_iommu_type1_dma_unmap unmap = {
		.argsz = sizeof(unmap),
		.iova = iova,
		.size = size,
	};
	int r;

	do {
		r = ioctl(container, VFIO_IOMMU_UNMAP_DMA, &unmap);
	} while (r < 0 && errno == EINTR);

	return r < 0 ? -errno : (int64_t)unmap.size;
}

int uml_vfio_user_get_group_id(const char *device)
{
	char *path, *buf, *end;
	const char *name;
	int r;

	path = uml_kmalloc(PATH_MAX, UM_GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	sprintf(path, "/sys/bus/pci/devices/%s/iommu_group", device);

	buf = uml_kmalloc(PATH_MAX + 1, UM_GFP_KERNEL);
	if (!buf) {
		r = -ENOMEM;
		goto free_path;
	}

	r = readlink(path, buf, PATH_MAX);
	if (r < 0) {
		r = -errno;
		goto free_buf;
	}
	buf[r] = '\0';

	name = basename(buf);

	r = strtoul(name, &end, 10);
	if (*end != '\0' || end == name) {
		r = -EINVAL;
		goto free_buf;
	}

free_buf:
	kfree(buf);
free_path:
	kfree(path);
	return r;
}

int uml_vfio_user_open_group(int group_id)
{
	char *path;
	int fd;

	path = uml_kmalloc(PATH_MAX, UM_GFP_KERNEL);
	if (!path)
		return -ENOMEM;

	sprintf(path, "/dev/vfio/%d", group_id);

	fd = open(path, O_RDWR);
	if (fd < 0) {
		fd = -errno;
		goto out;
	}

out:
	kfree(path);
	return fd;
}

int uml_vfio_user_set_container(int container, int group)
{
	if (ioctl(group, VFIO_GROUP_SET_CONTAINER, &container) < 0)
		return -errno;
	return 0;
}

int uml_vfio_user_unset_container(int container, int group)
{
	if (ioctl(group, VFIO_GROUP_UNSET_CONTAINER, &container) < 0)
		return -errno;
	return 0;
}

static int vfio_irq_index(int mode)
{
	return mode == UML_VFIO_IRQ_MSI ? VFIO_PCI_MSI_IRQ_INDEX :
					  VFIO_PCI_MSIX_IRQ_INDEX;
}

static int vfio_set_irqs(int device, struct vfio_irq_set *irq_set, int mode,
			 int start, int count, const int32_t *irqfd)
{
	irq_set->argsz = sizeof(*irq_set) + sizeof(*irqfd) * count;
	irq_set->flags = VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->flags |= irqfd ? VFIO_IRQ_SET_DATA_EVENTFD :
				  VFIO_IRQ_SET_DATA_NONE;
	irq_set->index = vfio_irq_index(mode);
	irq_set->start = start;
	irq_set->count = count;
	if (irqfd)
		memcpy(irq_set->data, irqfd, sizeof(*irqfd) * count);

	if (ioctl(device, VFIO_DEVICE_SET_IRQS, irq_set) < 0)
		return -errno;
	return 0;
}

static int vfio_get_irq_count(int device, unsigned int index)
{
	struct vfio_irq_info irq_info = {
		.argsz = sizeof(irq_info),
		.index = index,
	};

	if (ioctl(device, VFIO_DEVICE_GET_IRQ_INFO, &irq_info) < 0)
		return -errno;
	return irq_info.count;
}

int uml_vfio_user_setup_device(struct uml_vfio_user_device *dev,
			       int group, const char *device)
{
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	int err, i;

	dev->device = ioctl(group, VFIO_GROUP_GET_DEVICE_FD, device);
	if (dev->device < 0)
		return -errno;

	if (ioctl(dev->device, VFIO_DEVICE_GET_INFO, &device_info) < 0) {
		err = -errno;
		goto close_device;
	}

	dev->num_regions = device_info.num_regions;
	if (dev->num_regions > VFIO_PCI_CONFIG_REGION_INDEX + 1)
		dev->num_regions = VFIO_PCI_CONFIG_REGION_INDEX + 1;

	dev->region = uml_kmalloc(sizeof(*dev->region) * dev->num_regions,
				  UM_GFP_KERNEL);
	if (!dev->region) {
		err = -ENOMEM;
		goto close_device;
	}

	for (i = 0; i < dev->num_regions; i++) {
		struct vfio_region_info region = {
			.argsz = sizeof(region),
			.index = i,
		};
		if (ioctl(dev->device, VFIO_DEVICE_GET_REGION_INFO, &region) < 0) {
			err = -errno;
			goto free_region;
		}
		dev->region[i].size = region.size;
		dev->region[i].offset = region.offset;
	}

	dev->msix_count = vfio_get_irq_count(dev->device,
					     VFIO_PCI_MSIX_IRQ_INDEX);
	dev->msi_count = vfio_get_irq_count(dev->device, VFIO_PCI_MSI_IRQ_INDEX);
	if (dev->msix_count < 0 || dev->msi_count < 0) {
		err = dev->msix_count < 0 ? dev->msix_count : dev->msi_count;
		goto free_region;
	}

	/* Only single-vector MSI is supported. */
	if (dev->msi_count > 1)
		dev->msi_count = 1;

	dev->irq_count = dev->msix_count > dev->msi_count ?
			 dev->msix_count : dev->msi_count;
	if (!dev->irq_count) {
		err = -ENODEV;
		goto free_region;
	}

	dev->irqfd = uml_kmalloc(sizeof(int32_t) * dev->irq_count,
				 UM_GFP_KERNEL);
	if (!dev->irqfd) {
		err = -ENOMEM;
		goto free_region;
	}

	memset(dev->irqfd, -1, sizeof(int32_t) * dev->irq_count);

	dev->irq_set = uml_kmalloc(sizeof(struct vfio_irq_set) +
				   sizeof(int32_t) * dev->irq_count,
				   UM_GFP_KERNEL);
	if (!dev->irq_set) {
		err = -ENOMEM;
		goto free_irqfd;
	}

	return 0;

free_irqfd:
	kfree(dev->irqfd);
free_region:
	kfree(dev->region);
close_device:
	close(dev->device);
	return err;
}

void uml_vfio_user_teardown_device(struct uml_vfio_user_device *dev)
{
	kfree(dev->irq_set);
	kfree(dev->irqfd);
	kfree(dev->region);
	close(dev->device);
}

int uml_vfio_user_activate_irq(struct uml_vfio_user_device *dev, int index)
{
	int irqfd;

	irqfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (irqfd < 0)
		return -errno;

	dev->irqfd[index] = irqfd;
	return irqfd;
}

void uml_vfio_user_deactivate_irq(struct uml_vfio_user_device *dev, int index)
{
	close(dev->irqfd[index]);
	dev->irqfd[index] = -1;
}

static int vfio_mode_irq_count(struct uml_vfio_user_device *dev, int mode)
{
	return mode == UML_VFIO_IRQ_MSI ? dev->msi_count : dev->msix_count;
}

/*
 * Enable the interrupt mode on the host and set the eventfds of all its
 * vectors. This does not allocate, since it is called with a raw spinlock
 * held.
 */
int uml_vfio_user_enable_irqs(struct uml_vfio_user_device *dev, int mode)
{
	return vfio_set_irqs(dev->device, dev->irq_set, mode, 0,
			     vfio_mode_irq_count(dev, mode), dev->irqfd);
}

/* Set the eventfd of one vector of an enabled interrupt mode. */
int uml_vfio_user_update_irq(struct uml_vfio_user_device *dev, int mode,
			     int index)
{
	union {
		struct vfio_irq_set set;
		char buf[sizeof(struct vfio_irq_set) + sizeof(int32_t)];
	} irq_set;

	if (index >= vfio_mode_irq_count(dev, mode))
		return -EINVAL;

	return vfio_set_irqs(dev->device, &irq_set.set, mode, index, 1,
			     &dev->irqfd[index]);
}

int uml_vfio_user_disable_irqs(struct uml_vfio_user_device *dev, int mode)
{
	struct vfio_irq_set irq_set;

	return vfio_set_irqs(dev->device, &irq_set, mode, 0, 0, NULL);
}

static int vfio_region_read(struct uml_vfio_user_device *dev, unsigned int index,
			    uint64_t offset, void *buf, uint64_t size)
{
	if (index >= dev->num_regions || offset + size > dev->region[index].size)
		return -EINVAL;

	if (pread(dev->device, buf, size, dev->region[index].offset + offset) < 0)
		return -errno;

	return 0;
}

static int vfio_region_write(struct uml_vfio_user_device *dev, unsigned int index,
			     uint64_t offset, const void *buf, uint64_t size)
{
	if (index >= dev->num_regions || offset + size > dev->region[index].size)
		return -EINVAL;

	if (pwrite(dev->device, buf, size, dev->region[index].offset + offset) < 0)
		return -errno;

	return 0;
}

int uml_vfio_user_cfgspace_read(struct uml_vfio_user_device *dev,
				unsigned int offset, void *buf, int size)
{
	return vfio_region_read(dev, VFIO_PCI_CONFIG_REGION_INDEX,
				offset, buf, size);
}

int uml_vfio_user_cfgspace_write(struct uml_vfio_user_device *dev,
				 unsigned int offset, const void *buf, int size)
{
	return vfio_region_write(dev, VFIO_PCI_CONFIG_REGION_INDEX,
				 offset, buf, size);
}

int uml_vfio_user_bar_read(struct uml_vfio_user_device *dev, int bar,
			   unsigned int offset, void *buf, int size)
{
	return vfio_region_read(dev, bar, offset, buf, size);
}

int uml_vfio_user_bar_write(struct uml_vfio_user_device *dev, int bar,
			    unsigned int offset, const void *buf, int size)
{
	return vfio_region_write(dev, bar, offset, buf, size);
}

#define PAGE_ALIGN_DOWN(x) ((x) & ~((uint64_t)UM_KERN_PAGE_SIZE - 1))
#define PAGE_ALIGN_UP(x) PAGE_ALIGN_DOWN((x) + UM_KERN_PAGE_SIZE - 1)

static struct vfio_region_info *vfio_get_region_info(int device,
						     unsigned int index)
{
	struct vfio_region_info *info;
	uint32_t argsz = sizeof(*info);

	for (;;) {
		info = uml_kmalloc(argsz, UM_GFP_KERNEL);
		if (!info)
			return NULL;

		memset(info, 0, argsz);
		info->argsz = argsz;
		info->index = index;

		if (ioctl(device, VFIO_DEVICE_GET_REGION_INFO, info) < 0) {
			kfree(info);
			return NULL;
		}
		if (info->argsz <= argsz)
			return info;

		argsz = info->argsz;
		kfree(info);
	}
}

static struct vfio_region_info_cap_sparse_mmap *
vfio_find_sparse_mmap(struct vfio_region_info *info)
{
	struct vfio_info_cap_header *hdr;
	uint32_t off;

	if (!(info->flags & VFIO_REGION_INFO_FLAG_CAPS))
		return NULL;

	for (off = info->cap_offset; off; off = hdr->next) {
		if (off + sizeof(*hdr) > info->argsz)
			return NULL;
		hdr = (void *)info + off;
		if (hdr->id == VFIO_REGION_INFO_CAP_SPARSE_MMAP)
			return (void *)hdr;
	}

	return NULL;
}

static int vfio_range_contains(const struct uml_vfio_user_range *range,
			       uint64_t offset)
{
	return offset >= PAGE_ALIGN_DOWN(range->offset) &&
	       offset < PAGE_ALIGN_UP(range->offset + range->size);
}

/*
 * Each page of the BAR must be backed by either the device or a shadow
 * range, otherwise accesses to it cannot be handled.
 */
static int vfio_bar_covered(uint64_t size,
			    const struct vfio_region_sparse_mmap_area *areas,
			    int nr_areas,
			    const struct uml_vfio_user_range *shadow,
			    int nr_shadow)
{
	uint64_t off;
	int i, found;

	for (off = 0; off < size; off += UM_KERN_PAGE_SIZE) {
		found = 0;
		for (i = 0; i < nr_areas && !found; i++)
			found = off >= areas[i].offset &&
				off < areas[i].offset + areas[i].size;
		for (i = 0; i < nr_shadow && !found; i++)
			found = vfio_range_contains(&shadow[i], off);
		if (!found)
			return 0;
	}

	return 1;
}

void *uml_vfio_user_map_bar(struct uml_vfio_user_device *dev, int bar,
			    const struct uml_vfio_user_range *shadow,
			    int nr_shadow)
{
	struct vfio_region_info_cap_sparse_mmap *sparse;
	struct vfio_region_sparse_mmap_area whole, *areas;
	struct vfio_region_info *info;
	void *base = NULL, *p;
	uint64_t size, start;
	int i, nr_areas;

	info = vfio_get_region_info(dev->device, bar);
	if (!info)
		return NULL;

	if (!(info->flags & VFIO_REGION_INFO_FLAG_MMAP) || !info->size)
		goto out;

	size = PAGE_ALIGN_UP(info->size);

	for (i = 0; i < nr_shadow; i++) {
		if (shadow[i].offset > info->size ||
		    shadow[i].size > info->size - shadow[i].offset)
			goto out;
	}

	sparse = vfio_find_sparse_mmap(info);
	if (sparse) {
		areas = sparse->areas;
		nr_areas = sparse->nr_areas;
	} else {
		whole.offset = 0;
		whole.size = size;
		areas = &whole;
		nr_areas = 1;
	}

	if (!vfio_bar_covered(size, areas, nr_areas, shadow, nr_shadow))
		goto out;

	base = mmap(NULL, size, PROT_NONE,
		    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (base == MAP_FAILED) {
		base = NULL;
		goto out;
	}

	for (i = 0; i < nr_areas; i++) {
		if (!areas[i].size)
			continue;
		p = mmap(base + areas[i].offset, areas[i].size,
			 PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
			 dev->device, info->offset + areas[i].offset);
		if (p == MAP_FAILED)
			goto unmap;
	}

	for (i = 0; i < nr_shadow; i++) {
		start = PAGE_ALIGN_DOWN(shadow[i].offset);
		p = mmap(base + start,
			 PAGE_ALIGN_UP(shadow[i].offset + shadow[i].size) - start,
			 PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (p == MAP_FAILED)
			goto unmap;
	}

	goto out;

unmap:
	munmap(base, size);
	base = NULL;
out:
	kfree(info);
	return base;
}

void uml_vfio_user_unmap_bar(struct uml_vfio_user_device *dev, int bar,
			     void *addr)
{
	munmap(addr, PAGE_ALIGN_UP(dev->region[bar].size));
}
