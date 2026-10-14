// SPDX-License-Identifier: GPL-2.0
/*
 * Platform device passthrough for UML over vfio-platform.
 *
 * A host device bound to vfio-platform (e.g. an ACPI xHCI controller)
 * is recreated inside UML as a platform device of the same shape: one
 * IORESOURCE_MEM per VFIO region and one IORESOURCE_IRQ per VFIO
 * interrupt, bound by name to an ordinary platform driver (xhci-hcd).
 *
 * MMIO: each region gets a window in a logic_iomem range reserved for
 * these devices. Regions the host lets us mmap() are handed out as
 * direct mappings; the rest (vfio-platform only maps page-aligned
 * regions) are emulated with pread/pwrite on the device fd.
 *
 * Interrupts: platform interrupts are wired, normally level-triggered,
 * and vfio-platform automasks them each time they fire. Each one gets
 * an eventfd, serviced as a UML IRQ, which feeds a separate interrupt
 * descriptor that the driver requests. That descriptor runs
 * handle_fasteoi_irq on a chip whose eoi unmasks the host line, so the
 * host sees the same mask/handle/unmask sequence a real interrupt
 * controller would produce, and a line that is still asserted fires
 * again right after the unmask.
 *
 * DMA goes through the container and on-demand mapping shared with the
 * PCI side.
 */

#define pr_fmt(fmt) "vfio-uml: " fmt

#include <linux/dma-map-ops.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/list.h>
#include <linux/logic_iomem.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <uapi/linux/vfio.h>
#include <irq_kern.h>
#include <init.h>
#include <os.h>

#include "vfio_user.h"
#include "vfio_dma.h"
#include "vfio_kern.h"

struct uml_vfio_platform;

struct uml_vfio_platform_region {
	struct uml_vfio_platform *dev;
	int index;
	/* placement inside uml_vfio_platform_iomem, as an offset */
	unsigned long start;
	unsigned long size;
};

struct uml_vfio_platform_irq {
	struct uml_vfio_platform *dev;
	int index;
	int eventfd;
	int um_irq;		/* UML IRQ servicing the eventfd */
	int virq;		/* the interrupt the driver requests */
};

struct uml_vfio_platform {
	char *name;		/* host device, e.g. NVDA8000:02 */
	char *driver;		/* platform driver to bind, e.g. xhci-hcd */
	int group;

	struct uml_vfio_user_platform udev;
	struct uml_vfio_platform_region *regions;
	struct uml_vfio_platform_irq *irqs;
	struct platform_device *pdev;
	int dma_bits;		/* from the host's IORT, 0 if unlimited */

	struct list_head list;
};

/* Parsed from the command line, not yet opened. Boot is single-threaded. */
static LIST_HEAD(uml_vfio_platform_pending);

/*
 * Opened devices, whose regions the iomem map callback may look up. Never
 * held across platform_device_add(), which ioremap()s from the probe.
 */
static LIST_HEAD(uml_vfio_platform_devices);
static DEFINE_MUTEX(uml_vfio_platform_mtx);

/*
 * Guest-physical window the regions are placed in. The PCI side uses
 * 0xf0000000 and up (and its config space just below), the virtio
 * platform window is 0x10000000-0x1fffffff.
 */
static struct resource uml_vfio_platform_iomem = {
	.name = "VFIO platform iomem",
	.start = 0xd0000000,
	.end = 0xdfffffff,
	.flags = IORESOURCE_MEM,
};
static unsigned long uml_vfio_platform_iomem_next;

/*
 * Platform driver to bind when none is given, by the ACPI ids that show
 * up in the host device's modalias ("acpi:NVDA8000:PNP0D15:").
 */
static const struct {
	const char *id;
	const char *driver;
} uml_vfio_platform_drivers[] = {
	{ ":PNP0D10:", "xhci-hcd" },	/* xHCI */
	{ ":PNP0D15:", "xhci-hcd" },	/* xHCI without debug capability */
};

/* ---- MMIO ---------------------------------------------------------- */

static unsigned long uml_vfio_platform_read(void *priv, unsigned int offset,
					    int size)
{
	struct uml_vfio_platform_region *r = priv;
	u8 data[8];

	memset(data, 0xff, sizeof(data));
	uml_vfio_user_platform_read(&r->dev->udev, r->index, offset, data, size);

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

static void uml_vfio_platform_write(void *priv, unsigned int offset, int size,
				    unsigned long val)
{
	struct uml_vfio_platform_region *r = priv;
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
	default:
		return;
	}

	uml_vfio_user_platform_write(&r->dev->udev, r->index, offset, data, size);
}

static void uml_vfio_platform_set(void *priv, unsigned int offset, u8 value,
				  int size)
{
	struct uml_vfio_platform_region *r = priv;
	int i;

	for (i = 0; i < size; i++)
		uml_vfio_user_platform_write(&r->dev->udev, r->index,
					     offset + i, &value, 1);
}

static void uml_vfio_platform_copy_from(void *priv, void *buffer,
					unsigned int offset, int size)
{
	struct uml_vfio_platform_region *r = priv;

	memset(buffer, 0xff, size);
	uml_vfio_user_platform_read(&r->dev->udev, r->index, offset,
				    buffer, size);
}

static void uml_vfio_platform_copy_to(void *priv, unsigned int offset,
				      const void *buffer, int size)
{
	struct uml_vfio_platform_region *r = priv;

	uml_vfio_user_platform_write(&r->dev->udev, r->index, offset,
				     buffer, size);
}

static const struct logic_iomem_ops uml_vfio_platform_region_ops = {
	.read		= uml_vfio_platform_read,
	.write		= uml_vfio_platform_write,
	.set		= uml_vfio_platform_set,
	.copy_from	= uml_vfio_platform_copy_from,
	.copy_to	= uml_vfio_platform_copy_to,
};

static long uml_vfio_platform_map(unsigned long offset, size_t size,
				  const struct logic_iomem_ops **ops,
				  void **priv)
{
	struct uml_vfio_platform *dev;
	int i;

	guard(mutex)(&uml_vfio_platform_mtx);

	list_for_each_entry(dev, &uml_vfio_platform_devices, list) {
		for (i = 0; i < dev->udev.num_regions; i++) {
			struct uml_vfio_platform_region *r = &dev->regions[i];
			void *map = dev->udev.region[i].map;
			unsigned long off;

			/* must be the whole or part of one region */
			if (offset < r->start || offset - r->start >= r->size ||
			    size > r->size - (offset - r->start))
				continue;

			off = offset - r->start;

			if (IS_ENABLED(CONFIG_INDIRECT_IOMEM_FALLBACK) && map) {
				/* direct mapping, see logic_iomem */
				*ops = NULL;
				*priv = map + off;
				return 0;
			}

			*ops = &uml_vfio_platform_region_ops;
			*priv = r;
			return off;
		}
	}

	WARN(1, "cannot map offset 0x%lx/0x%zx\n", offset, size);
	return -ENOENT;
}

static const struct logic_iomem_region_ops uml_vfio_platform_iomem_ops = {
	.map = uml_vfio_platform_map,
};

static int uml_vfio_platform_place_regions(struct uml_vfio_platform *dev)
{
	unsigned long window = resource_size(&uml_vfio_platform_iomem);
	int i;

	dev->regions = kzalloc_objs(*dev->regions, dev->udev.num_regions);
	if (!dev->regions)
		return -ENOMEM;

	for (i = 0; i < dev->udev.num_regions; i++) {
		struct uml_vfio_platform_region *r = &dev->regions[i];
		unsigned long size = dev->udev.region[i].size;

		if (!size || size > window ||
		    uml_vfio_platform_iomem_next > window - PAGE_ALIGN(size))
			return -ENOSPC;

		r->dev = dev;
		r->index = i;
		r->start = uml_vfio_platform_iomem_next;
		r->size = size;
		uml_vfio_platform_iomem_next += PAGE_ALIGN(size);
	}

	return 0;
}

/* ---- interrupts ---------------------------------------------------- */

static void uml_vfio_platform_irq_mask(struct irq_data *d)
{
	struct uml_vfio_platform_irq *irq = irq_data_get_irq_chip_data(d);

	WARN_ON_ONCE(uml_vfio_user_platform_mask_irq(&irq->dev->udev,
						     irq->index, 1));
}

static void uml_vfio_platform_irq_unmask(struct irq_data *d)
{
	struct uml_vfio_platform_irq *irq = irq_data_get_irq_chip_data(d);

	WARN_ON_ONCE(uml_vfio_user_platform_mask_irq(&irq->dev->udev,
						     irq->index, 0));
}

/*
 * End of a handled interrupt. vfio-platform automasked the line when it
 * fired, so this is where it gets unmasked again -- unless the core is
 * keeping it masked (disabled, or a oneshot thread still to run), in
 * which case the core's later irq_unmask does it.
 */
static void uml_vfio_platform_irq_eoi(struct irq_data *d)
{
	if (!irqd_irq_masked(d))
		uml_vfio_platform_irq_unmask(d);
}

static struct irq_chip uml_vfio_platform_irq_chip = {
	.name		= "vfio-platform",
	.irq_mask	= uml_vfio_platform_irq_mask,
	.irq_unmask	= uml_vfio_platform_irq_unmask,
	.irq_eoi	= uml_vfio_platform_irq_eoi,
};

static irqreturn_t uml_vfio_platform_interrupt(int unused, void *opaque)
{
	struct uml_vfio_platform_irq *irq = opaque;
	bool fired = false;
	uint64_t v;
	int r;

	do {
		r = os_read_file(irq->eventfd, &v, sizeof(v));
		if (r == sizeof(v))
			fired = true;
	} while (r == sizeof(v) || r == -EINTR);
	WARN(r != -EAGAIN, "read returned %d\n", r);

	/*
	 * The host line stays masked until our eoi, so the count can only
	 * be 1; one pass of the handler covers it either way.
	 */
	if (fired)
		generic_handle_irq(irq->virq);

	return IRQ_HANDLED;
}

static void uml_vfio_platform_free_irq(struct uml_vfio_platform_irq *irq)
{
	if (irq->um_irq >= 0) {
		ignore_sigio_fd(irq->eventfd);
		um_free_irq(irq->um_irq, irq);
	}
	if (irq->eventfd >= 0)
		uml_vfio_user_platform_deactivate_irq(&irq->dev->udev,
						      irq->index, irq->eventfd);
	if (irq->virq >= 0)
		irq_free_desc(irq->virq);

	irq->um_irq = irq->eventfd = irq->virq = -1;
}

static int uml_vfio_platform_setup_irq(struct uml_vfio_platform *dev,
				       int index)
{
	struct uml_vfio_platform_irq *irq = &dev->irqs[index];
	int err;

	irq->dev = dev;
	irq->index = index;
	irq->um_irq = irq->eventfd = irq->virq = -1;

	if (!(dev->udev.irq_flags[index] & VFIO_IRQ_INFO_AUTOMASKED))
		pr_warn("%s: interrupt %d is edge-triggered, which is untested\n",
			dev->name, index);

	irq->virq = irq_alloc_desc(numa_node_id());
	if (irq->virq < 0)
		return irq->virq;

	irq_set_chip_and_handler(irq->virq, &uml_vfio_platform_irq_chip,
				 handle_fasteoi_irq);
	irq_set_chip_data(irq->virq, irq);
	irq_set_status_flags(irq->virq, IRQ_LEVEL);

	/*
	 * The descriptor starts out masked, and stays so until the driver
	 * requests it. Anything the host signals before then finds no
	 * handler, so the eoi skips the unmask and the line waits, masked
	 * on the host, for the request.
	 */
	irq->eventfd = uml_vfio_user_platform_activate_irq(&dev->udev, index);
	if (irq->eventfd < 0) {
		err = irq->eventfd;
		goto free;
	}

	irq->um_irq = um_request_irq(UM_IRQ_ALLOC, irq->eventfd, IRQ_READ,
				     uml_vfio_platform_interrupt, 0,
				     "vfio-platform", irq);
	if (irq->um_irq < 0) {
		err = irq->um_irq;
		goto free;
	}

	err = add_sigio_fd(irq->eventfd);
	if (err)
		goto free;

	return 0;

free:
	uml_vfio_platform_free_irq(irq);
	return err;
}

/* ---- devices ------------------------------------------------------- */

static int uml_vfio_platform_guess_driver(struct uml_vfio_platform *dev)
{
	char modalias[128];
	int i, r;

	r = uml_vfio_user_platform_modalias(dev->name, modalias,
					    sizeof(modalias));
	if (r < 0)
		return r;

	for (i = 0; i < ARRAY_SIZE(uml_vfio_platform_drivers); i++) {
		if (strstr(modalias, uml_vfio_platform_drivers[i].id)) {
			dev->driver = kstrdup(uml_vfio_platform_drivers[i].driver,
					      GFP_KERNEL);
			return dev->driver ? 0 : -ENOMEM;
		}
	}

	pr_err("%s: no driver known for modalias '%s', give one with vfio_uml.platform=%s,<driver>\n",
	       dev->name, modalias, dev->name);
	return -ENODEV;
}

static int uml_vfio_platform_add_pdev(struct uml_vfio_platform *dev)
{
	int nregions = dev->udev.num_regions, nirqs = dev->udev.num_irqs;
	struct platform_device *pdev;
	struct resource *res;
	int err, i;

	res = kzalloc_objs(*res, nregions + nirqs);
	if (!res)
		return -ENOMEM;

	for (i = 0; i < nregions; i++) {
		res[i].start = uml_vfio_platform_iomem.start +
			       dev->regions[i].start;
		res[i].end = res[i].start + dev->regions[i].size - 1;
		res[i].flags = IORESOURCE_MEM;
	}
	for (i = 0; i < nirqs; i++) {
		res[nregions + i].start = dev->irqs[i].virq;
		res[nregions + i].end = dev->irqs[i].virq;
		res[nregions + i].flags = IORESOURCE_IRQ;
	}

	pdev = platform_device_alloc(dev->driver, PLATFORM_DEVID_AUTO);
	if (!pdev) {
		err = -ENOMEM;
		goto free_res;
	}

	err = platform_device_add_resources(pdev, res, nregions + nirqs);
	if (err)
		goto put_pdev;

	if (!uml_vfio_eager_dma)
		set_dma_ops(&pdev->dev, &uml_vfio_dma_ops);
	if (dev->dma_bits)
		pdev->dev.bus_dma_limit = DMA_BIT_MASK(dev->dma_bits);

	err = platform_device_add(pdev);
	if (err)
		goto put_pdev;

	dev->pdev = pdev;
	kfree(res);
	return 0;

put_pdev:
	platform_device_put(pdev);
free_res:
	kfree(res);
	return err;
}

static void uml_vfio_platform_free(struct uml_vfio_platform *dev)
{
	kfree(dev->driver);
	kfree(dev->name);
	kfree(dev);
}

static void uml_vfio_platform_close(struct uml_vfio_platform *dev)
{
	int i;

	if (dev->pdev)
		platform_device_unregister(dev->pdev);

	scoped_guard(mutex, &uml_vfio_platform_mtx)
		list_del(&dev->list);

	for (i = 0; i < dev->udev.num_irqs; i++)
		uml_vfio_platform_free_irq(&dev->irqs[i]);
	kfree(dev->irqs);
	kfree(dev->regions);
	uml_vfio_user_teardown_platform(&dev->udev);
	uml_vfio_release_group(dev->group);
	uml_vfio_platform_free(dev);
}

static int uml_vfio_platform_open(struct uml_vfio_platform *dev)
{
	int err, group_id, i;

	if (!dev->driver) {
		err = uml_vfio_platform_guess_driver(dev);
		if (err)
			return err;
	}

	group_id = uml_vfio_user_get_group_id("platform", dev->name);
	if (group_id < 0) {
		pr_err("%s: failed to get group id, error %d\n",
		       dev->name, group_id);
		return group_id;
	}

	dev->group = uml_vfio_open_group(group_id);
	if (dev->group < 0) {
		pr_err("%s: failed to open group %d, error %d\n",
		       dev->name, group_id, dev->group);
		return dev->group;
	}

	err = uml_vfio_user_setup_platform(&dev->udev, dev->group, dev->name);
	if (err) {
		pr_err("%s: failed to set up device, error %d%s\n",
		       dev->name, err, err == -EPERM ?
		       " (the host may need iommufd allow_unsafe_interrupts=1)" : "");
		goto release_group;
	}

	err = uml_vfio_dma_ready();
	if (err) {
		pr_err("%s: failed to set up DMA, error %d\n", dev->name, err);
		goto teardown_udev;
	}

	dev->dma_bits = uml_vfio_user_iort_dma_bits("platform", dev->name);
	if (dev->dma_bits)
		pr_info("%s: DMA limited to %d bits by the host's IORT\n",
			dev->name, dev->dma_bits);

	err = uml_vfio_platform_place_regions(dev);
	if (err) {
		pr_err("%s: cannot place %d region(s), error %d\n",
		       dev->name, dev->udev.num_regions, err);
		goto free_regions;
	}

	dev->irqs = kzalloc_objs(*dev->irqs, dev->udev.num_irqs);
	if (!dev->irqs) {
		err = -ENOMEM;
		goto free_regions;
	}

	for (i = 0; i < dev->udev.num_irqs; i++) {
		err = uml_vfio_platform_setup_irq(dev, i);
		if (err) {
			pr_err("%s: failed to set up interrupt %d, error %d\n",
			       dev->name, i, err);
			goto free_irqs;
		}
	}

	scoped_guard(mutex, &uml_vfio_platform_mtx)
		list_add_tail(&dev->list, &uml_vfio_platform_devices);

	err = uml_vfio_platform_add_pdev(dev);
	if (err) {
		pr_err("%s: failed to add platform device, error %d\n",
		       dev->name, err);
		scoped_guard(mutex, &uml_vfio_platform_mtx)
			list_del(&dev->list);
		goto free_irqs;
	}

	pr_info("%s: %d region(s), %d interrupt(s), bound as %s\n",
		dev->name, dev->udev.num_regions, dev->udev.num_irqs,
		dev_name(&dev->pdev->dev));
	return 0;

free_irqs:
	for (i = 0; i < dev->udev.num_irqs && dev->irqs[i].dev; i++)
		uml_vfio_platform_free_irq(&dev->irqs[i]);
	kfree(dev->irqs);
free_regions:
	kfree(dev->regions);
teardown_udev:
	uml_vfio_user_teardown_platform(&dev->udev);
release_group:
	uml_vfio_release_group(dev->group);
	return err;
}

void uml_vfio_platform_init(void)
{
	struct uml_vfio_platform *dev, *n;

	if (list_empty(&uml_vfio_platform_pending))
		return;

	if (WARN_ON(logic_iomem_add_region(&uml_vfio_platform_iomem,
					   &uml_vfio_platform_iomem_ops)))
		return;

	list_for_each_entry_safe(dev, n, &uml_vfio_platform_pending, list) {
		list_del(&dev->list);
		if (uml_vfio_platform_open(dev))
			uml_vfio_platform_free(dev);
	}
}

void uml_vfio_platform_exit(void)
{
	struct uml_vfio_platform *dev, *n;

	list_for_each_entry_safe(dev, n, &uml_vfio_platform_devices, list)
		uml_vfio_platform_close(dev);
}

static int uml_vfio_platform_cmdline_set(const char *val,
					 const struct kernel_param *kp)
{
	struct uml_vfio_platform *dev;
	char *comma;
	int err;

	err = uml_vfio_open_container();
	if (err)
		return err;

	dev = kzalloc_obj(*dev);
	if (!dev)
		return -ENOMEM;

	dev->name = kstrdup(val, GFP_KERNEL);
	if (!dev->name)
		goto nomem;

	comma = strchr(dev->name, ',');
	if (comma) {
		*comma = '\0';
		dev->driver = kstrdup(comma + 1, GFP_KERNEL);
		if (!dev->driver)
			goto nomem;
	}

	list_add_tail(&dev->list, &uml_vfio_platform_pending);
	return 0;

nomem:
	uml_vfio_platform_free(dev);
	return -ENOMEM;
}

static int uml_vfio_platform_cmdline_get(char *buffer,
					 const struct kernel_param *kp)
{
	return 0;
}

static const struct kernel_param_ops uml_vfio_platform_cmdline_param_ops = {
	.set = uml_vfio_platform_cmdline_set,
	.get = uml_vfio_platform_cmdline_get,
};

device_param_cb(platform, &uml_vfio_platform_cmdline_param_ops, NULL, 0400);
__uml_help(uml_vfio_platform_cmdline_param_ops,
"vfio_uml.platform=<device>[,<driver>]\n"
"    Pass through a host platform device bound to vfio-platform, e.g.\n"
"    vfio_uml.platform=NVDA8000:02. It is recreated as a platform device\n"
"    with the same regions and interrupts and bound to <driver>; ACPI\n"
"    xHCI controllers (PNP0D10/PNP0D15) default to xhci-hcd. This\n"
"    parameter can be specified multiple times.\n\n"
);
