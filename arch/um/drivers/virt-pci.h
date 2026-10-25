/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VIRT_PCI_H
#define __UM_VIRT_PCI_H

#include <linux/pci.h>

struct um_pci_device {
	const struct um_pci_ops *ops;
	/* Optional DMA ops for the device, dma-direct is used if NULL. */
	const struct dma_map_ops *dma_ops;

	/* The standard BARs and the expansion ROM. */
	u8 resptr[PCI_ROM_RESOURCE + 1];

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
	 * Optional. Return a pointer to a direct mapping of the BAR, or NULL
	 * to access it through the ops above.
	 */
	void __iomem *(*bar_map)(struct um_pci_device *dev, int bar);

	/*
	 * Optional. Reset the device, or with probe, return 0 if it can be
	 * reset. The guest uses it as a slot reset.
	 */
	int (*reset)(struct um_pci_device *dev, bool probe);

	/* Optional. Called when an MSI or MSI-X vector is set up or torn down. */
	int (*msi_activate)(struct um_pci_device *dev, unsigned int index,
			    unsigned int irq);
	void (*msi_deactivate)(struct um_pci_device *dev, unsigned int index);
	/* Optional. Called when a vector is masked or unmasked. */
	void (*msi_mask)(struct um_pci_device *dev, unsigned int index,
			 bool masked);
	/*
	 * Optional. Resend an interrupt on a vector. Return true if the
	 * interrupt will be delivered again.
	 */
	bool (*msi_retrigger)(struct um_pci_device *dev, unsigned int index);
	/*
	 * Optional. Handle a vector's interrupts on a CPU. Without it, the
	 * vector's affinity cannot be set.
	 */
	void (*msi_set_cpu)(struct um_pci_device *dev, unsigned int index,
			    unsigned int cpu);
};

int um_pci_device_register(struct um_pci_device *dev);
void um_pci_device_unregister(struct um_pci_device *dev);

int um_pci_platform_device_register(struct um_pci_device *dev);
void um_pci_platform_device_unregister(struct um_pci_device *dev);

#endif /* __UM_VIRT_PCI_H */
