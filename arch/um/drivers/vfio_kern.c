// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Ant Group
 * Author: Tiwei Bie <tiwei.btw@antgroup.com>
 */

#define pr_fmt(fmt) "vfio-uml: " fmt

#include <linux/module.h>
#include <linux/logic_iomem.h>
#include <linux/mutex.h>
#include <linux/kstrtox.h>
#include <linux/log2.h>
#include <linux/list.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <irq_kern.h>
#include <init.h>
#include <os.h>

#include "mconsole_kern.h"
#include "virt-pci.h"
#include "vfio_dma.h"
#include "vfio_user.h"

#define to_vdev(_pdev) container_of(_pdev, struct uml_vfio_device, pdev)

struct uml_vfio_intr_ctx {
	struct uml_vfio_device *dev;
	int irq;
	int virq;
	/* The CPU that handles the vector, which its eventfd's IRQ follows. */
	unsigned int cpu;
	bool masked;
	bool pending;

	/*
	 * Counters, protected by irq_lock. Signals is the sum of the eventfd
	 * counts read, and each read is either delivered to the driver or
	 * deferred because the vector is masked. Replays are deferred or
	 * resent interrupts written back to the eventfd.
	 */
	u64 signals;
	u64 delivered;
	u64 deferred;
	u64 replays;
};

/* Accesses that are forwarded to the VFIO device fd. */
struct uml_vfio_access_stats {
	atomic64_t cfg_reads;
	atomic64_t cfg_writes;
	atomic64_t bar_reads;
	atomic64_t bar_writes;
};

/*
 * Guest memory that stands in for a part of a BAR, used for the MSI-X table
 * and PBA so that the driver never writes the device's real table.
 */
struct uml_vfio_shadow {
	int bar;
	unsigned int offset;
	unsigned int size;
	void *mem;
};

enum {
	UML_VFIO_SHADOW_TABLE,
	UML_VFIO_SHADOW_PBA,
	UML_VFIO_NR_SHADOWS,
};

struct uml_vfio_device {
	const char *name;
	int group;
	/* The group fd given on the command line, or -1. */
	int passed_group;

	struct um_pci_device pdev;
	struct uml_vfio_user_device udev;
	struct uml_vfio_intr_ctx *intr_ctx;

	void *bar_map[PCI_STD_NUM_BARS];
	struct uml_vfio_shadow shadow[UML_VFIO_NR_SHADOWS];
	int nr_shadows;

	int msi_cap;
	int msix_cap;
	int msix_entries;
	bool msix_maskall;
	/* The interrupt mode enabled on the host. */
	int irq_mode;
	/* Protects irq_mode and the interrupt configuration on the host. */
	raw_spinlock_t mode_lock;

	/*
	 * When MSI-X is hidden from the guest, reads of the capability pointer
	 * at msix_ptr return msix_next instead.
	 */
	bool msix_hidden;
	u8 msix_ptr;
	u8 msix_next;

	/* The fd of the ROM image given on the command line, or -1. */
	int rom_fd;
	/*
	 * The ROM image, padded to the size of the ROM BAR, and the emulated
	 * ROM BAR register. When it is set, the image replaces the device's
	 * ROM.
	 */
	void *rom;
	u32 rom_size;
	u32 rom_bar;
	/* Protects the mask state of the device and its vectors. */
	raw_spinlock_t irq_lock;

	struct uml_vfio_access_stats stats;
	/* Set while the device is registered, for the proc file. */
	bool registered;

	struct list_head list;
};

struct uml_vfio_group {
	int id;
	int fd;
	int users;
	struct list_head list;
};

static struct {
	int fd;
	int users;
} uml_vfio_container = { .fd = -1 };
static DEFINE_MUTEX(uml_vfio_container_mtx);

static LIST_HEAD(uml_vfio_groups);
static DEFINE_MUTEX(uml_vfio_groups_mtx);

static LIST_HEAD(uml_vfio_devices);
static DEFINE_MUTEX(uml_vfio_devices_mtx);

static bool uml_vfio_no_msix;
module_param_named(no_msix, uml_vfio_no_msix, bool, 0400);

static int uml_vfio_container_fd = -1;
module_param_named(container_fd, uml_vfio_container_fd, int, 0400);

/* Open the container, or check the one given on the command line. */
static int uml_vfio_get_container(void)
{
	int err;

	lockdep_assert_held(&uml_vfio_container_mtx);

	if (uml_vfio_container.fd >= 0)
		return 0;

	if (uml_vfio_container_fd < 0) {
		err = uml_vfio_user_open_container();
		if (err < 0) {
			pr_err("Failed to open container, error %d\n", err);
			return err;
		}
		uml_vfio_container.fd = err;
		return 0;
	}

	err = uml_vfio_user_check_container(uml_vfio_container_fd);
	if (err) {
		pr_err("Failed to use container fd %d, error %d\n",
		       uml_vfio_container_fd, err);
		return err;
	}
	uml_vfio_container.fd = uml_vfio_container_fd;
	return 0;
}

static int uml_vfio_set_container(int group_fd)
{
	int err;

	guard(mutex)(&uml_vfio_container_mtx);

	err = uml_vfio_get_container();
	if (err)
		return err;

	err = uml_vfio_user_set_container(uml_vfio_container.fd, group_fd);
	if (err)
		return err;

	uml_vfio_container.users++;
	if (uml_vfio_container.users == 1)
		err = uml_vfio_user_setup_iommu(uml_vfio_container.fd);
	if (!err)
		err = uml_vfio_dma_add_group(uml_vfio_container.fd);
	if (err) {
		uml_vfio_user_unset_container(uml_vfio_container.fd, group_fd);
		uml_vfio_container.users--;
		if (!uml_vfio_container.users)
			uml_vfio_dma_release();
	}
	return err;
}

static void uml_vfio_unset_container(int group_fd)
{
	guard(mutex)(&uml_vfio_container_mtx);

	uml_vfio_user_unset_container(uml_vfio_container.fd, group_fd);
	uml_vfio_container.users--;
	if (!uml_vfio_container.users)
		uml_vfio_dma_release();
}

/*
 * Return the fd of a group, given by its id or, if fd is not negative, by an
 * open fd. The group is attached to the container when it is first used.
 */
static int uml_vfio_open_group(int group_id, int fd)
{
	struct uml_vfio_group *group;
	int err;

	guard(mutex)(&uml_vfio_groups_mtx);

	list_for_each_entry(group, &uml_vfio_groups, list) {
		if (fd >= 0 ? group->fd == fd : group->id == group_id) {
			group->users++;
			return group->fd;
		}
	}

	group = kzalloc_obj(*group);
	if (!group) {
		if (fd >= 0)
			os_close_file(fd);
		return -ENOMEM;
	}

	group->fd = fd >= 0 ? fd : uml_vfio_user_open_group(group_id);
	if (group->fd < 0) {
		err = group->fd;
		goto free_group;
	}

	err = uml_vfio_set_container(group->fd);
	if (err)
		goto close_group;

	group->id = group_id;
	group->users = 1;

	list_add(&group->list, &uml_vfio_groups);

	return group->fd;

close_group:
	os_close_file(group->fd);
free_group:
	kfree(group);
	return err;
}

static int uml_vfio_release_group(int group_fd)
{
	struct uml_vfio_group *group;

	guard(mutex)(&uml_vfio_groups_mtx);

	list_for_each_entry(group, &uml_vfio_groups, list) {
		if (group->fd == group_fd) {
			group->users--;
			if (group->users == 0) {
				uml_vfio_unset_container(group_fd);
				os_close_file(group_fd);
				list_del(&group->list);
				kfree(group);
			}
			return 0;
		}
	}

	return -ENOENT;
}

static bool uml_vfio_irq_masked(struct uml_vfio_intr_ctx *ctx)
{
	return ctx->masked || ctx->dev->msix_maskall;
}

/*
 * Replay an interrupt by writing to the eventfd, so that it is delivered
 * through the usual path.
 */
static void uml_vfio_retrigger_irq(struct uml_vfio_intr_ctx *ctx)
{
	struct uml_vfio_device *dev = ctx->dev;
	int irqfd = dev->udev.irqfd[ctx - dev->intr_ctx];
	uint64_t v = 1;

	lockdep_assert_held(&dev->irq_lock);

	ctx->pending = false;
	if (ctx->irq >= 0) {
		WARN_ON(os_write_file(irqfd, &v, sizeof(v)) != sizeof(v));
		ctx->replays++;
	}
}

static irqreturn_t uml_vfio_interrupt(int unused, void *opaque)
{
	struct uml_vfio_intr_ctx *ctx = opaque;
	struct uml_vfio_device *dev = ctx->dev;
	int index = ctx - dev->intr_ctx;
	int irqfd = dev->udev.irqfd[index];
	unsigned long flags;
	bool masked;
	uint64_t v;
	int r;

	/*
	 * A read returns and clears the whole count. The eventfd is
	 * edge-triggered in the IRQ epoll set, so a signal after the read
	 * raises a new event, and a second read would only fail.
	 */
	do {
		r = os_read_file(irqfd, &v, sizeof(v));
	} while (r == -EINTR);

	if (r == sizeof(v)) {
		raw_spin_lock_irqsave(&dev->irq_lock, flags);
		masked = uml_vfio_irq_masked(ctx);
		if (masked)
			ctx->pending = true;
		ctx->signals += v;
		if (masked)
			ctx->deferred++;
		else
			ctx->delivered++;
		raw_spin_unlock_irqrestore(&dev->irq_lock, flags);

		if (!masked)
			generic_handle_irq(ctx->virq);
	} else {
		WARN(r != -EAGAIN, "read returned %d\n", r);
	}

	return IRQ_HANDLED;
}

static int uml_vfio_activate_irq(struct uml_vfio_device *dev, int index)
{
	struct uml_vfio_intr_ctx *ctx = &dev->intr_ctx[index];
	int err, irqfd;

	if (ctx->irq >= 0)
		return 0;

	irqfd = uml_vfio_user_activate_irq(&dev->udev, index);
	if (irqfd < 0)
		return irqfd;

	ctx->irq = um_request_irq(UM_IRQ_ALLOC, irqfd, IRQ_READ,
				  uml_vfio_interrupt, 0,
				  "vfio-uml", ctx);
	if (ctx->irq < 0) {
		err = ctx->irq;
		goto deactivate;
	}

	err = add_sigio_fd(irqfd);
	if (err)
		goto free_irq;

	um_irq_set_cpu(ctx->irq, READ_ONCE(ctx->cpu));
	return 0;

free_irq:
	um_free_irq(ctx->irq, ctx);
	ctx->irq = -1;
deactivate:
	uml_vfio_user_deactivate_irq(&dev->udev, index);
	return err;
}

static int uml_vfio_deactivate_irq(struct uml_vfio_device *dev, int index)
{
	struct uml_vfio_intr_ctx *ctx = &dev->intr_ctx[index];

	if (ctx->irq >= 0) {
		ignore_sigio_fd(dev->udev.irqfd[index]);
		um_free_irq(ctx->irq, ctx);
		uml_vfio_user_deactivate_irq(&dev->udev, index);
		ctx->irq = -1;
	}
	return 0;
}

static void uml_vfio_set_maskall(struct uml_vfio_device *dev, bool maskall)
{
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&dev->irq_lock, flags);
	dev->msix_maskall = maskall;
	for (i = 0; i < dev->udev.irq_count; i++) {
		struct uml_vfio_intr_ctx *ctx = &dev->intr_ctx[i];

		if (ctx->pending && !uml_vfio_irq_masked(ctx))
			uml_vfio_retrigger_irq(ctx);
	}
	raw_spin_unlock_irqrestore(&dev->irq_lock, flags);
}

/*
 * Switch the host to the interrupt mode the driver enabled and set the
 * eventfds of its vectors. VFIO cannot enable one mode while another is
 * enabled, so the old mode is disabled first. Setting the eventfds again
 * when the mode is unchanged rearms them after the driver re-enables
 * interrupts.
 */
static int uml_vfio_set_irq_mode(struct uml_vfio_device *dev, int mode)
{
	int old = dev->irq_mode;
	int err;

	lockdep_assert_held(&dev->mode_lock);

	if (old != mode && old != UML_VFIO_IRQ_NONE) {
		err = uml_vfio_user_disable_irqs(&dev->udev, old);
		if (err)
			return err;
	}

	dev->irq_mode = mode;
	if (mode == UML_VFIO_IRQ_NONE)
		return 0;

	err = uml_vfio_user_enable_irqs(&dev->udev, mode);
	/* VFIO leaves a mode that it fails to enable disabled. */
	if (err && old != mode)
		dev->irq_mode = UML_VFIO_IRQ_NONE;
	return err;
}

static int uml_vfio_update_msix_cap(struct uml_vfio_device *dev,
				    unsigned int offset, int size,
				    unsigned long val)
{
	bool enabled = val & PCI_MSIX_FLAGS_ENABLE;
	int err = 0;

	/*
	 * Here, we handle only the operations we care about,
	 * ignoring the rest.
	 */
	if (size != 2 || offset != dev->msix_cap + PCI_MSIX_FLAGS)
		return 0;

	guard(raw_spinlock_irqsave)(&dev->mode_lock);

	if (enabled)
		err = uml_vfio_set_irq_mode(dev, UML_VFIO_IRQ_MSIX);
	else if (dev->irq_mode == UML_VFIO_IRQ_MSIX)
		err = uml_vfio_set_irq_mode(dev, UML_VFIO_IRQ_NONE);

	uml_vfio_set_maskall(dev, enabled && (val & PCI_MSIX_FLAGS_MASKALL));
	return err;
}

static int uml_vfio_update_msi_cap(struct uml_vfio_device *dev,
				   unsigned int offset, int size,
				   unsigned long val)
{
	if (size != 2 || offset != dev->msi_cap + PCI_MSI_FLAGS)
		return 0;

	guard(raw_spinlock_irqsave)(&dev->mode_lock);

	if (val & PCI_MSI_FLAGS_ENABLE)
		return uml_vfio_set_irq_mode(dev, UML_VFIO_IRQ_MSI);
	if (dev->irq_mode == UML_VFIO_IRQ_MSI)
		return uml_vfio_set_irq_mode(dev, UML_VFIO_IRQ_NONE);
	return 0;
}

static bool uml_vfio_is_rom_bar(struct uml_vfio_device *dev,
				unsigned int offset, int size)
{
	return dev->rom && offset >= PCI_ROM_ADDRESS &&
	       offset + size <= PCI_ROM_ADDRESS + 4;
}

static unsigned long uml_vfio_read_rom_bar(struct uml_vfio_device *dev,
					   unsigned int offset, int size)
{
	u32 val = dev->rom_bar >> ((offset - PCI_ROM_ADDRESS) * 8);

	if (size < 4)
		val &= (1U << (size * 8)) - 1;
	return val;
}

/*
 * Only the address bits that fit the image's size and the enable bit can be
 * written, so a sizing write reads back the size of the image.
 */
static void uml_vfio_write_rom_bar(struct uml_vfio_device *dev,
				   unsigned int offset, int size,
				   unsigned long val)
{
	unsigned int shift = (offset - PCI_ROM_ADDRESS) * 8;
	u32 mask = size < 4 ? ((1U << (size * 8)) - 1) << shift : ~0U;
	u32 bar = (dev->rom_bar & ~mask) | ((u32)(val << shift) & mask);

	dev->rom_bar = bar & ((~(dev->rom_size - 1) & PCI_ROM_ADDRESS_MASK) |
			      PCI_ROM_ADDRESS_ENABLE);
}

static unsigned long __uml_vfio_cfgspace_read(struct uml_vfio_device *dev,
					      unsigned int offset, int size)
{
	u8 data[8];

	memset(data, 0xff, sizeof(data));

	if (uml_vfio_user_cfgspace_read(&dev->udev, offset, data, size))
		return ULONG_MAX;

	switch (size) {
	case 1:
		return data[0];
	case 2:
		return le16_to_cpup((void *)data);
	case 4:
		return le32_to_cpup((void *)data);
#ifdef CONFIG_64BIT
	case 8:
		return le64_to_cpup((void *)data);
#endif
	default:
		return ULONG_MAX;
	}
}

static unsigned long uml_vfio_cfgspace_read(struct um_pci_device *pdev,
					    unsigned int offset, int size)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	unsigned long val;
	int shift;

	if (uml_vfio_is_rom_bar(dev, offset, size))
		return uml_vfio_read_rom_bar(dev, offset, size);

	val = __uml_vfio_cfgspace_read(dev, offset, size);
	atomic64_inc(&dev->stats.cfg_reads);

	if (dev->msix_hidden && dev->msix_ptr >= offset &&
	    dev->msix_ptr < offset + size) {
		shift = (dev->msix_ptr - offset) * 8;
		val &= ~(0xffUL << shift);
		val |= (unsigned long)dev->msix_next << shift;
	}

	/*
	 * Each device is function 0 of its own slot, so it is never shown as
	 * multi-function. The PCI core does not offer a slot reset for a
	 * multi-function device.
	 */
	if (PCI_HEADER_TYPE >= offset && PCI_HEADER_TYPE < offset + size) {
		shift = (PCI_HEADER_TYPE - offset) * 8;
		val &= ~((unsigned long)PCI_HEADER_TYPE_MFD << shift);
	}

	return val;
}

static void __uml_vfio_cfgspace_write(struct uml_vfio_device *dev,
				      unsigned int offset, int size,
				      unsigned long val)
{
	u8 data[8];

	switch (size) {
	case 1:
		data[0] = (u8)val;
		break;
	case 2:
		put_unaligned_le16(val, (void *)data);
		break;
	case 4:
		put_unaligned_le32(val, (void *)data);
		break;
#ifdef CONFIG_64BIT
	case 8:
		put_unaligned_le64(val, (void *)data);
		break;
#endif
	}

	WARN_ON(uml_vfio_user_cfgspace_write(&dev->udev, offset, data, size));
}

static void uml_vfio_cfgspace_write(struct um_pci_device *pdev,
				    unsigned int offset, int size,
				    unsigned long val)
{
	struct uml_vfio_device *dev = to_vdev(pdev);

	if (uml_vfio_is_rom_bar(dev, offset, size)) {
		uml_vfio_write_rom_bar(dev, offset, size, val);
		return;
	}

	atomic64_inc(&dev->stats.cfg_writes);

	if (dev->msix_cap)
		WARN_ON(uml_vfio_update_msix_cap(dev, offset, size, val));
	if (dev->msi_cap)
		WARN_ON(uml_vfio_update_msi_cap(dev, offset, size, val));

	__uml_vfio_cfgspace_write(dev, offset, size, val);
}

static void *uml_vfio_find_shadow(struct uml_vfio_device *dev, int bar,
				  unsigned int offset, int size)
{
	int i;

	for (i = 0; i < dev->nr_shadows; i++) {
		struct uml_vfio_shadow *shadow = &dev->shadow[i];

		if (shadow->mem && shadow->bar == bar &&
		    offset >= shadow->offset &&
		    offset + size <= shadow->offset + shadow->size)
			return shadow->mem + offset - shadow->offset;
	}

	return NULL;
}

static void uml_vfio_bar_copy_from(struct um_pci_device *pdev, int bar,
				   void *buffer, unsigned int offset, int size)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	void *shadow = uml_vfio_find_shadow(dev, bar, offset, size);

	if (shadow) {
		memcpy(buffer, shadow, size);
		return;
	}

	if (bar == PCI_ROM_RESOURCE && dev->rom) {
		if (offset < dev->rom_size && size <= dev->rom_size - offset)
			memcpy(buffer, dev->rom + offset, size);
		else
			memset(buffer, 0xff, size);
		return;
	}

	atomic64_inc(&dev->stats.bar_reads);
	memset(buffer, 0xff, size);
	uml_vfio_user_bar_read(&dev->udev, bar, offset, buffer, size);
}

static unsigned long uml_vfio_bar_read(struct um_pci_device *pdev, int bar,
				       unsigned int offset, int size)
{
	u8 data[8];

	uml_vfio_bar_copy_from(pdev, bar, data, offset, size);

	switch (size) {
	case 1:
		return data[0];
	case 2:
		return le16_to_cpup((void *)data);
	case 4:
		return le32_to_cpup((void *)data);
#ifdef CONFIG_64BIT
	case 8:
		return le64_to_cpup((void *)data);
#endif
	default:
		return ULONG_MAX;
	}
}

static void uml_vfio_bar_copy_to(struct um_pci_device *pdev, int bar,
				 unsigned int offset, const void *buffer,
				 int size)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	void *shadow = uml_vfio_find_shadow(dev, bar, offset, size);

	if (shadow) {
		memcpy(shadow, buffer, size);
		return;
	}

	/* A supplied ROM image is read-only, like a real ROM. */
	if (bar == PCI_ROM_RESOURCE && dev->rom)
		return;

	atomic64_inc(&dev->stats.bar_writes);
	uml_vfio_user_bar_write(&dev->udev, bar, offset, buffer, size);
}

static void uml_vfio_bar_write(struct um_pci_device *pdev, int bar,
			       unsigned int offset, int size,
			       unsigned long val)
{
	u8 data[8];

	switch (size) {
	case 1:
		data[0] = (u8)val;
		break;
	case 2:
		put_unaligned_le16(val, (void *)data);
		break;
	case 4:
		put_unaligned_le32(val, (void *)data);
		break;
#ifdef CONFIG_64BIT
	case 8:
		put_unaligned_le64(val, (void *)data);
		break;
#endif
	}

	uml_vfio_bar_copy_to(pdev, bar, offset, data, size);
}

static void uml_vfio_bar_set(struct um_pci_device *pdev, int bar,
			     unsigned int offset, u8 value, int size)
{
	int i;

	for (i = 0; i < size; i++)
		uml_vfio_bar_copy_to(pdev, bar, offset + i, &value, 1);
}

/*
 * VFIO resets the device with whatever method the host has for it, which may
 * be a bus reset that the guest cannot do itself.
 */
static int uml_vfio_reset(struct um_pci_device *pdev, bool probe)
{
	struct uml_vfio_device *dev = to_vdev(pdev);

	if (!dev->udev.can_reset)
		return -ENOTTY;
	if (probe)
		return 0;
	return uml_vfio_user_reset(&dev->udev);
}

static void __iomem *uml_vfio_bar_map(struct um_pci_device *pdev, int bar)
{
	struct uml_vfio_device *dev = to_vdev(pdev);

	if (bar == PCI_ROM_RESOURCE)
		return (void __iomem __force *)dev->rom;
	if (bar >= PCI_STD_NUM_BARS)
		return NULL;
	return (void __iomem __force *)dev->bar_map[bar];
}

/*
 * The first activation of a vector comes from MSI_FLAG_ACTIVATE_EARLY when
 * the vectors are allocated, which may sleep. Later activations from
 * request_irq() hold the irq descriptor lock and reuse the existing eventfd.
 */
static int uml_vfio_msi_activate(struct um_pci_device *pdev,
				 unsigned int index, unsigned int irq)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	struct uml_vfio_intr_ctx *ctx;
	unsigned long flags;
	int err, mode;
	bool created;

	if (index >= dev->udev.irq_count)
		return -EINVAL;

	ctx = &dev->intr_ctx[index];

	raw_spin_lock_irqsave(&dev->irq_lock, flags);
	ctx->virq = irq;
	ctx->masked = true;
	ctx->pending = false;
	raw_spin_unlock_irqrestore(&dev->irq_lock, flags);

	created = ctx->irq < 0;
	err = uml_vfio_activate_irq(dev, index);
	if (err || !created)
		return err;

	/*
	 * A vector created while its mode is enabled gets its eventfd here.
	 * While MSI-X is masked as a whole, the write that unmasks it sets the
	 * eventfds of every vector instead.
	 */
	guard(raw_spinlock_irqsave)(&dev->mode_lock);
	mode = dev->irq_mode;
	if (mode == UML_VFIO_IRQ_NONE ||
	    (mode == UML_VFIO_IRQ_MSIX && dev->msix_maskall))
		return 0;
	return uml_vfio_user_update_irq(&dev->udev, mode, index);
}

/*
 * Deactivation can run under the irq descriptor lock from free_irq(), so the
 * eventfd is kept until the device is released. This also keeps the eventfd
 * that VFIO signals valid if the vector is activated again.
 */
static void uml_vfio_msi_deactivate(struct um_pci_device *pdev,
				    unsigned int index)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	struct uml_vfio_intr_ctx *ctx;
	unsigned long flags;

	if (index >= dev->udev.irq_count)
		return;

	ctx = &dev->intr_ctx[index];

	raw_spin_lock_irqsave(&dev->irq_lock, flags);
	ctx->masked = true;
	ctx->pending = false;
	raw_spin_unlock_irqrestore(&dev->irq_lock, flags);
}

static void uml_vfio_msi_mask(struct um_pci_device *pdev, unsigned int index,
			      bool masked)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	struct uml_vfio_intr_ctx *ctx;
	unsigned long flags;

	if (index >= dev->udev.irq_count)
		return;

	ctx = &dev->intr_ctx[index];

	raw_spin_lock_irqsave(&dev->irq_lock, flags);
	ctx->masked = masked;
	if (ctx->pending && !uml_vfio_irq_masked(ctx))
		uml_vfio_retrigger_irq(ctx);
	raw_spin_unlock_irqrestore(&dev->irq_lock, flags);
}

/*
 * Called by the irq core to resend an interrupt that arrived while it was
 * disabled. A masked vector records it as pending instead.
 */
static bool uml_vfio_msi_retrigger(struct um_pci_device *pdev,
				   unsigned int index)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	struct uml_vfio_intr_ctx *ctx;
	unsigned long flags;
	bool ret;

	if (index >= dev->udev.irq_count)
		return false;

	ctx = &dev->intr_ctx[index];

	raw_spin_lock_irqsave(&dev->irq_lock, flags);
	ret = ctx->irq >= 0;
	if (uml_vfio_irq_masked(ctx))
		ctx->pending = true;
	else
		uml_vfio_retrigger_irq(ctx);
	raw_spin_unlock_irqrestore(&dev->irq_lock, flags);

	return ret;
}

/*
 * Move a vector's eventfd to the IRQ epoll set of a CPU, or remember the CPU
 * for when the vector is activated.
 */
static void uml_vfio_msi_set_cpu(struct um_pci_device *pdev,
				 unsigned int index, unsigned int cpu)
{
	struct uml_vfio_device *dev = to_vdev(pdev);
	struct uml_vfio_intr_ctx *ctx;

	if (index >= dev->udev.irq_count)
		return;

	ctx = &dev->intr_ctx[index];
	WRITE_ONCE(ctx->cpu, cpu);
	if (ctx->irq >= 0)
		um_irq_set_cpu(ctx->irq, cpu);
}

static const struct um_pci_ops uml_vfio_um_pci_ops = {
	.cfgspace_read	= uml_vfio_cfgspace_read,
	.cfgspace_write	= uml_vfio_cfgspace_write,
	.bar_read	= uml_vfio_bar_read,
	.bar_write	= uml_vfio_bar_write,
	.bar_copy_from	= uml_vfio_bar_copy_from,
	.bar_copy_to	= uml_vfio_bar_copy_to,
	.bar_set	= uml_vfio_bar_set,
	.bar_map	= uml_vfio_bar_map,
	.reset		= uml_vfio_reset,
	.msi_activate	= uml_vfio_msi_activate,
	.msi_deactivate	= uml_vfio_msi_deactivate,
	.msi_mask	= uml_vfio_msi_mask,
	.msi_retrigger	= uml_vfio_msi_retrigger,
	.msi_set_cpu	= uml_vfio_msi_set_cpu,
};

/*
 * Return the offset of the capability, and store the offset of the pointer
 * to it in ptr.
 */
static u8 uml_vfio_find_capability(struct uml_vfio_device *dev, u8 cap,
				   u8 *ptr)
{
	u8 id, pos, prev = PCI_CAPABILITY_LIST;
	u16 ent;
	int ttl = 48; /* PCI_FIND_CAP_TTL */

	pos = __uml_vfio_cfgspace_read(dev, PCI_CAPABILITY_LIST, sizeof(pos));

	while (pos && ttl--) {
		ent = __uml_vfio_cfgspace_read(dev, pos, sizeof(ent));

		id = ent & 0xff;
		if (id == 0xff)
			break;
		if (id == cap) {
			if (ptr)
				*ptr = prev;
			return pos;
		}

		prev = pos + PCI_CAP_LIST_NEXT;
		pos = ent >> 8;
	}

	return 0;
}

static int uml_vfio_read_msix_cap(struct uml_vfio_device *dev)
{
	unsigned int off;
	u32 tbl, pba;
	u16 flags;
	u8 ptr;

	off = uml_vfio_find_capability(dev, PCI_CAP_ID_MSIX, &ptr);
	if (!off)
		return 0;

	if (uml_vfio_no_msix || !dev->udev.msix_count) {
		dev->msix_hidden = true;
		dev->msix_ptr = ptr;
		dev->msix_next = __uml_vfio_cfgspace_read(dev,
							  off + PCI_CAP_LIST_NEXT,
							  sizeof(u8));
		return 0;
	}

	dev->msix_cap = off;
	dev->nr_shadows = UML_VFIO_NR_SHADOWS;

	tbl = __uml_vfio_cfgspace_read(dev, off + PCI_MSIX_TABLE, sizeof(tbl));
	pba = __uml_vfio_cfgspace_read(dev, off + PCI_MSIX_PBA, sizeof(pba));
	flags = __uml_vfio_cfgspace_read(dev, off + PCI_MSIX_FLAGS, sizeof(flags));

	dev->msix_entries = (flags & PCI_MSIX_FLAGS_QSIZE) + 1;

	dev->shadow[UML_VFIO_SHADOW_TABLE] = (struct uml_vfio_shadow) {
		.bar = tbl & PCI_MSIX_TABLE_BIR,
		.offset = tbl & PCI_MSIX_TABLE_OFFSET,
		.size = dev->msix_entries * PCI_MSIX_ENTRY_SIZE,
	};
	dev->shadow[UML_VFIO_SHADOW_PBA] = (struct uml_vfio_shadow) {
		.bar = pba & PCI_MSIX_PBA_BIR,
		.offset = pba & PCI_MSIX_PBA_OFFSET,
		.size = DIV_ROUND_UP(dev->msix_entries, 64) * sizeof(u64),
	};

	if (dev->shadow[UML_VFIO_SHADOW_TABLE].bar >= PCI_STD_NUM_BARS ||
	    dev->shadow[UML_VFIO_SHADOW_PBA].bar >= PCI_STD_NUM_BARS)
		return -EINVAL;

	return 0;
}

static int uml_vfio_read_caps(struct uml_vfio_device *dev)
{
	int err;

	err = uml_vfio_read_msix_cap(dev);
	if (err)
		return err;

	if (dev->udev.msi_count)
		dev->msi_cap = uml_vfio_find_capability(dev, PCI_CAP_ID_MSI,
							NULL);

	if (!dev->msix_cap && !dev->msi_cap)
		return -ENODEV;
	return 0;
}

static bool uml_vfio_bar_is_mem(struct uml_vfio_device *dev, int bar)
{
	u32 val;

	val = __uml_vfio_cfgspace_read(dev, PCI_BASE_ADDRESS_0 + bar * 4,
				       sizeof(val));
	return (val & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY;
}

static void uml_vfio_unmap_bars(struct uml_vfio_device *dev)
{
	int i;

	for (i = 0; i < dev->nr_shadows; i++) {
		struct uml_vfio_shadow *shadow = &dev->shadow[i];

		if (!dev->bar_map[shadow->bar])
			kfree(shadow->mem);
		shadow->mem = NULL;
	}

	for (i = 0; i < PCI_STD_NUM_BARS; i++) {
		if (dev->bar_map[i])
			uml_vfio_user_unmap_bar(&dev->udev, i, dev->bar_map[i]);
		dev->bar_map[i] = NULL;
	}
}

/*
 * Map each memory BAR directly, with the MSI-X table and PBA backed by
 * shadow memory. BARs that cannot be mapped are accessed through the VFIO
 * device fd, with the shadows in kernel memory.
 */
static int uml_vfio_map_bars(struct uml_vfio_device *dev)
{
	struct uml_vfio_user_range ranges[UML_VFIO_NR_SHADOWS];
	struct uml_vfio_shadow *shadow;
	int bar, i, n;

	for (bar = 0; bar < PCI_STD_NUM_BARS; bar++) {
		if (bar >= dev->udev.num_regions || !dev->udev.region[bar].size)
			continue;
		if (!uml_vfio_bar_is_mem(dev, bar))
			continue;

		n = 0;
		for (i = 0; i < dev->nr_shadows; i++) {
			shadow = &dev->shadow[i];
			if (shadow->bar != bar)
				continue;
			ranges[n].offset = shadow->offset;
			ranges[n].size = shadow->size;
			n++;
		}

		dev->bar_map[bar] = uml_vfio_user_map_bar(&dev->udev, bar,
							  ranges, n);
		if (dev->bar_map[bar])
			continue;

		pr_warn("BAR %d of %s cannot be mapped, using trapped access\n",
			bar, dev->name);
		/* logic_iomem reaches at most 16M of a trapped mapping. */
		if (dev->udev.region[bar].size > SZ_16M)
			pr_warn("BAR %d of %s is larger than 16M, and trapped accesses past 16M are not supported\n",
				bar, dev->name);
	}

	for (i = 0; i < dev->nr_shadows; i++) {
		shadow = &dev->shadow[i];
		if (dev->bar_map[shadow->bar])
			shadow->mem = dev->bar_map[shadow->bar] + shadow->offset;
		else
			shadow->mem = kzalloc(shadow->size, GFP_KERNEL);
		if (!shadow->mem) {
			uml_vfio_unmap_bars(dev);
			return -ENOMEM;
		}
	}

	/* Vectors are masked at reset. */
	for (i = 0; dev->nr_shadows && i < dev->msix_entries; i++)
		put_unaligned_le32(PCI_MSIX_ENTRY_CTRL_MASKBIT,
				   dev->shadow[UML_VFIO_SHADOW_TABLE].mem +
				   i * PCI_MSIX_ENTRY_SIZE +
				   PCI_MSIX_ENTRY_VECTOR_CTRL);

	return 0;
}

/*
 * Read the ROM image from rom_fd, which is closed afterwards. The image is
 * padded with zeros to the size of the ROM BAR, a power of two of at least
 * 2K.
 */
static int uml_vfio_load_rom(struct uml_vfio_device *dev)
{
	unsigned long long off = 0;
	struct uml_stat st;
	int err, n;

	err = os_stat_fd(dev->rom_fd, &st);
	if (err)
		goto out;

	if (!st.ust_size || st.ust_size > SZ_16M) {
		err = -EINVAL;
		goto out;
	}

	dev->rom_size = max(roundup_pow_of_two(st.ust_size), SZ_2K);
	dev->rom = kvzalloc(dev->rom_size, GFP_KERNEL);
	if (!dev->rom) {
		err = -ENOMEM;
		goto out;
	}

	while (off < st.ust_size) {
		n = os_pread_file(dev->rom_fd, dev->rom + off,
				  st.ust_size - off, off);
		if (n == -EINTR)
			continue;
		if (n <= 0) {
			err = n ?: -EIO;
			kvfree(dev->rom);
			dev->rom = NULL;
			goto out;
		}
		off += n;
	}

out:
	os_close_file(dev->rom_fd);
	dev->rom_fd = -1;
	return err;
}

static void uml_vfio_open_device(struct uml_vfio_device *dev)
{
	struct uml_vfio_intr_ctx *ctx;
	int err, group_id = -1, i;

	if (dev->rom_fd >= 0) {
		err = uml_vfio_load_rom(dev);
		if (err) {
			pr_err("Failed to read ROM image (%s), error %d\n",
			       dev->name, err);
			goto free_dev;
		}
	}

	if (dev->passed_group < 0) {
		group_id = uml_vfio_user_get_group_id(dev->name);
		if (group_id < 0) {
			pr_err("Failed to get group id (%s), error %d\n",
			       dev->name, group_id);
			goto free_dev;
		}
	}

	dev->group = uml_vfio_open_group(group_id, dev->passed_group);
	if (dev->group < 0) {
		if (dev->passed_group < 0)
			pr_err("Failed to open group %d (%s), error %d\n",
			       group_id, dev->name, dev->group);
		else
			pr_err("Failed to use group fd %d (%s), error %d\n",
			       dev->passed_group, dev->name, dev->group);
		goto free_dev;
	}

	err = uml_vfio_user_setup_device(&dev->udev, dev->group, dev->name);
	if (err) {
		pr_err("Failed to setup device (%s), error %d\n",
		       dev->name, err);
		goto release_group;
	}

	err = uml_vfio_read_caps(dev);
	if (err) {
		pr_err("Failed to find MSI or MSI-X capability (%s), error %d\n",
		       dev->name, err);
		goto teardown_udev;
	}

	err = uml_vfio_map_bars(dev);
	if (err) {
		pr_err("Failed to map BARs (%s), error %d\n",
		       dev->name, err);
		goto teardown_udev;
	}

	dev->intr_ctx = kzalloc_objs(struct uml_vfio_intr_ctx,
				     dev->udev.irq_count);
	if (!dev->intr_ctx) {
		pr_err("Failed to allocate interrupt context (%s)\n",
		       dev->name);
		goto unmap_bars;
	}

	raw_spin_lock_init(&dev->irq_lock);
	raw_spin_lock_init(&dev->mode_lock);
	for (i = 0; i < dev->udev.irq_count; i++) {
		ctx = &dev->intr_ctx[i];
		ctx->dev = dev;
		ctx->irq = -1;
		ctx->masked = true;
		ctx->pending = false;
	}

	dev->pdev.ops = &uml_vfio_um_pci_ops;
	dev->pdev.dma_ops = &uml_vfio_dma_ops;

	err = um_pci_device_register(&dev->pdev);
	if (err) {
		pr_err("Failed to register UM PCI device (%s), error %d\n",
		       dev->name, err);
		goto free_intr_ctx;
	}

	scoped_guard(mutex, &uml_vfio_devices_mtx)
		dev->registered = true;
	return;

free_intr_ctx:
	kfree(dev->intr_ctx);
unmap_bars:
	uml_vfio_unmap_bars(dev);
teardown_udev:
	uml_vfio_user_teardown_device(&dev->udev);
release_group:
	uml_vfio_release_group(dev->group);
free_dev:
	scoped_guard(mutex, &uml_vfio_devices_mtx)
		list_del(&dev->list);
	kvfree(dev->rom);
	kfree(dev->name);
	kfree(dev);
}

static void uml_vfio_release_device(struct uml_vfio_device *dev)
{
	int i;

	scoped_guard(mutex, &uml_vfio_devices_mtx)
		dev->registered = false;

	scoped_guard(raw_spinlock_irqsave, &dev->mode_lock)
		uml_vfio_set_irq_mode(dev, UML_VFIO_IRQ_NONE);
	for (i = 0; i < dev->udev.irq_count; i++)
		uml_vfio_deactivate_irq(dev, i);

	um_pci_device_unregister(&dev->pdev);
	kfree(dev->intr_ctx);
	uml_vfio_unmap_bars(dev);
	uml_vfio_user_teardown_device(&dev->udev);
	uml_vfio_release_group(dev->group);
	scoped_guard(mutex, &uml_vfio_devices_mtx)
		list_del(&dev->list);
	kvfree(dev->rom);
	kfree(dev->name);
	kfree(dev);
}

static struct uml_vfio_device *uml_vfio_find_device(const char *device)
{
	struct uml_vfio_device *dev;

	list_for_each_entry(dev, &uml_vfio_devices, list) {
		if (!strcmp(dev->name, device))
			return dev;
	}
	return NULL;
}

/* Parse an option of the form <key><n>, where n is an fd. */
static int uml_vfio_parse_fd(const char *opt, const char *key, int *fd)
{
	if (!strstarts(opt, key))
		return -ENOENT;
	if (kstrtoint(opt + strlen(key), 10, fd) || *fd < 0)
		return -EINVAL;
	return 0;
}

/*
 * Add a device given as
 * <domain:bus:slot.function>[,group_fd=<n>][,rom_fd=<n>].
 */
static struct uml_vfio_device *uml_vfio_add_device(const char *arg)
{
	struct uml_vfio_device *dev;
	int group_fd = -1, rom_fd = -1;
	char *name, *opts, *opt;
	int err;

	name = kstrdup(arg, GFP_KERNEL);
	if (!name)
		return ERR_PTR(-ENOMEM);

	/* The name ends at the first comma, which strsep() overwrites. */
	opts = name;
	strsep(&opts, ",");
	while ((opt = strsep(&opts, ","))) {
		err = uml_vfio_parse_fd(opt, "group_fd=", &group_fd);
		if (err == -ENOENT)
			err = uml_vfio_parse_fd(opt, "rom_fd=", &rom_fd);
		if (err) {
			kfree(name);
			return ERR_PTR(-EINVAL);
		}
	}

	guard(mutex)(&uml_vfio_devices_mtx);

	if (uml_vfio_find_device(name)) {
		kfree(name);
		return ERR_PTR(-EEXIST);
	}

	dev = kzalloc_obj(*dev);
	if (!dev) {
		kfree(name);
		return ERR_PTR(-ENOMEM);
	}

	dev->name = name;
	dev->passed_group = group_fd;
	dev->rom_fd = rom_fd;

	list_add_tail(&dev->list, &uml_vfio_devices);
	return dev;
}

static int uml_vfio_cmdline_set(const char *device, const struct kernel_param *kp)
{
	struct uml_vfio_device *dev;

	dev = uml_vfio_add_device(device);
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	return 0;
}

static int uml_vfio_cmdline_get(char *buffer, const struct kernel_param *kp)
{
	return 0;
}

static const struct kernel_param_ops uml_vfio_cmdline_param_ops = {
	.set = uml_vfio_cmdline_set,
	.get = uml_vfio_cmdline_get,
};

device_param_cb(device, &uml_vfio_cmdline_param_ops, NULL, 0400);
__uml_help(uml_vfio_cmdline_param_ops,
"vfio_uml.device=<domain:bus:slot.function>[,group_fd=<n>][,rom_fd=<n>]\n"
"    Pass through a PCI device to UML via VFIO. The device must support\n"
"    MSI-X or MSI. This parameter can be specified multiple times to pass\n"
"    through multiple PCI devices to UML. With group_fd, the device's\n"
"    group is the open host file descriptor <n> instead of the one opened\n"
"    from /dev/vfio. Devices in the same group take the same fd. With\n"
"    rom_fd, the guest sees the image in the open host file descriptor <n>\n"
"    as the device's expansion ROM, in place of any ROM the device has.\n"
"    The fd is closed after it is read.\n\n"
"vfio_uml.container_fd=<n>\n"
"    Use the open host file descriptor <n> as the VFIO container instead\n"
"    of opening /dev/vfio/vfio.\n\n"
"vfio_uml.no_msix=1\n"
"    Hide the MSI-X capability of passed-through devices, so that drivers\n"
"    use MSI.\n\n"
);

static int uml_vfio_mc_config(char *str, char **error_out)
{
	struct uml_vfio_device *dev;

	if (*str != '=') {
		*error_out = "Invalid config";
		return -EINVAL;
	}
	str += 1;

	dev = uml_vfio_add_device(str);
	if (IS_ERR(dev))
		return PTR_ERR(dev);
	uml_vfio_open_device(dev);
	return 0;
}

static int uml_vfio_mc_id(char **str, int *start_out, int *end_out)
{
	return -EOPNOTSUPP;
}

static int uml_vfio_mc_remove(int n, char **error_out)
{
	return -EOPNOTSUPP;
}

static struct mc_device uml_vfio_mc = {
	.list           = LIST_HEAD_INIT(uml_vfio_mc.list),
	.name           = "vfio_uml.device",
	.config         = uml_vfio_mc_config,
	.get_config     = NULL,
	.id             = uml_vfio_mc_id,
	.remove         = uml_vfio_mc_remove,
};

static const char *const uml_vfio_irq_mode_names[] = {
	[UML_VFIO_IRQ_NONE] = "none",
	[UML_VFIO_IRQ_MSI] = "msi",
	[UML_VFIO_IRQ_MSIX] = "msix",
};

static void uml_vfio_show_device(struct seq_file *m,
				 struct uml_vfio_device *dev)
{
	struct uml_vfio_access_stats *stats = &dev->stats;
	unsigned long flags;
	int i;

	seq_printf(m, "device %s\n", dev->name);
	seq_printf(m, "  %-18s %s\n", "irq_mode",
		   uml_vfio_irq_mode_names[READ_ONCE(dev->irq_mode)]);

	for (i = 0; i <= PCI_ROM_RESOURCE && i < dev->udev.num_regions; i++) {
		char name[8];

		if (!dev->udev.region[i].size ||
		    (i == PCI_ROM_RESOURCE && dev->rom))
			continue;
		if (i == PCI_ROM_RESOURCE)
			strscpy(name, "rom");
		else
			snprintf(name, sizeof(name), "bar%d", i);
		seq_printf(m, "  %-18s %s 0x%llx\n", name,
			   uml_vfio_bar_map(&dev->pdev, i) ? "direct" : "trapped",
			   (unsigned long long)dev->udev.region[i].size);
	}
	if (dev->rom)
		seq_printf(m, "  %-18s supplied 0x%x\n", "rom", dev->rom_size);

	seq_printf(m, "  %-18s %lld\n", "cfg_reads",
		   atomic64_read(&stats->cfg_reads));
	seq_printf(m, "  %-18s %lld\n", "cfg_writes",
		   atomic64_read(&stats->cfg_writes));
	seq_printf(m, "  %-18s %lld\n", "bar_reads",
		   atomic64_read(&stats->bar_reads));
	seq_printf(m, "  %-18s %lld\n", "bar_writes",
		   atomic64_read(&stats->bar_writes));

	seq_printf(m, "  %-6s %12s %12s %12s %12s\n", "vector", "signals",
		   "delivered", "deferred", "replays");
	for (i = 0; i < dev->udev.irq_count; i++) {
		struct uml_vfio_intr_ctx *ctx = &dev->intr_ctx[i];
		u64 signals, delivered, deferred, replays;

		if (ctx->irq < 0)
			continue;

		raw_spin_lock_irqsave(&dev->irq_lock, flags);
		signals = ctx->signals;
		delivered = ctx->delivered;
		deferred = ctx->deferred;
		replays = ctx->replays;
		raw_spin_unlock_irqrestore(&dev->irq_lock, flags);

		seq_printf(m, "  %-6d %12llu %12llu %12llu %12llu\n", i,
			   signals, delivered, deferred, replays);
	}
}

static int __maybe_unused uml_vfio_proc_show(struct seq_file *m, void *v)
{
	struct uml_vfio_device *dev;

	uml_vfio_dma_show(m);

	seq_puts(m, "iomem\n");
	seq_printf(m, "  %-18s %llu\n", "faulted_accesses",
		   (unsigned long long)uml_iomem_faults());

	guard(mutex)(&uml_vfio_devices_mtx);
	list_for_each_entry(dev, &uml_vfio_devices, list) {
		if (dev->registered)
			uml_vfio_show_device(m, dev);
	}

	return 0;
}

static int __init uml_vfio_init(void)
{
	struct uml_vfio_device *dev, *n;

	sigio_broken();

	/* If the opening fails, the device will be released. */
	list_for_each_entry_safe(dev, n, &uml_vfio_devices, list)
		uml_vfio_open_device(dev);

	mconsole_register_dev(&uml_vfio_mc);
	proc_create_single("vfio_uml", 0444, NULL, uml_vfio_proc_show);

	return 0;
}
late_initcall(uml_vfio_init);

static void __exit uml_vfio_exit(void)
{
	struct uml_vfio_device *dev, *n;

	remove_proc_entry("vfio_uml", NULL);

	list_for_each_entry_safe(dev, n, &uml_vfio_devices, list)
		uml_vfio_release_device(dev);

	if (uml_vfio_container.fd >= 0)
		os_close_file(uml_vfio_container.fd);
}
module_exit(uml_vfio_exit);
