/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VIRT_PCI_H
#define __UM_VIRT_PCI_H

#include <linux/pci.h>

struct dma_map_ops;

struct um_pci_device {
	const struct um_pci_ops *ops;

	/*
	 * Optional DMA ops the backend wants installed on the PCI
	 * device (e.g. VFIO on-demand mapping).  NULL keeps the
	 * default dma-direct path.
	 */
	const struct dma_map_ops *dma_ops;

	/* for now just standard BARs */
	u8 resptr[PCI_STD_NUM_BARS];

	int irq;
};

struct um_pci_ops {
	unsigned long (*cfgspace_read)(struct um_pci_device *dev,
				       unsigned int offset, int size);
	void (*cfgspace_write)(struct um_pci_device *dev, unsigned int offset,
			       int size, unsigned long val);

	unsigned long (*bar_read)(struct um_pci_device *dev, int bar,
				  unsigned int offset, int size);
	void (*bar_write)(struct um_pci_device *dev, int bar,
			  unsigned int offset, int size, unsigned long val);

	void (*bar_copy_from)(struct um_pci_device *dev, int bar, void *buffer,
			      unsigned int offset, int size);
	void (*bar_copy_to)(struct um_pci_device *dev, int bar,
			    unsigned int offset, const void *buffer, int size);
	void (*bar_set)(struct um_pci_device *dev, int bar,
			unsigned int offset, u8 value, int size);

	/*
	 * Optional: return a directly usable pointer for the given range
	 * of the BAR (e.g. into an mmap()ed VFIO region), or NULL if the
	 * range must be accessed through the emulated ops above. Only
	 * used with CONFIG_INDIRECT_IOMEM_FALLBACK.
	 */
	void __iomem *(*bar_map)(struct um_pci_device *dev, int bar,
				 unsigned int offset, size_t size);
};

int um_pci_device_register(struct um_pci_device *dev);
void um_pci_device_unregister(struct um_pci_device *dev);

int um_pci_platform_device_register(struct um_pci_device *dev);
void um_pci_platform_device_unregister(struct um_pci_device *dev);

#endif /* __UM_VIRT_PCI_H */
