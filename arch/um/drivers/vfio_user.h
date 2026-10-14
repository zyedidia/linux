/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VFIO_USER_H
#define __UM_VFIO_USER_H

/*
 * Interrupt mode currently programmed. Which one is used is the guest
 * driver's choice, not a property of the device: a driver may probe
 * MSI-X and fall back to MSI, so both are kept available and the mode
 * follows whichever capability the guest enables.
 */
enum uml_vfio_irq_type {
	UML_VFIO_IRQ_MSIX,
	UML_VFIO_IRQ_MSI,
};

struct uml_vfio_user_region {
	uint64_t size;
	uint64_t offset;
	/* host address the region is mmap()ed at, NULL if not */
	void *map;
};

struct uml_vfio_user_device {
	int device;

	struct uml_vfio_user_region *region;
	int num_regions;

	enum uml_vfio_irq_type irq_type;
	int msix_count;			/* vectors the device offers, 0 if none */
	int msi_count;			/* 1 if plain MSI is available */
	int32_t *irqfd;
	int irq_count;			/* entries in irqfd[] */
};

/*
 * A device bound to vfio-platform: some MMIO regions and some wired
 * interrupts, nothing else. The interrupts are typically level-triggered
 * and automasked by the host on each assertion, so the guest has to
 * unmask after handling one.
 */
struct uml_vfio_user_platform {
	int device;

	struct uml_vfio_user_region *region;
	int num_regions;

	int num_irqs;
	uint32_t *irq_flags;		/* VFIO_IRQ_INFO_* per interrupt */
};

struct uml_vfio_iova_range {
	uint64_t start;		/* first byte of the range */
	uint64_t end;		/* last byte of the range (inclusive) */
};

struct uml_vfio_iommu_info {
	uint64_t pgsizes;	/* bitmap of supported IOMMU page sizes */
	int nranges;		/* number of valid IOVA ranges */
	struct uml_vfio_iova_range *ranges;	/* kfree() when done */
};

int uml_vfio_user_open_container(void);
int uml_vfio_user_setup_iommu(int container);
int uml_vfio_user_map_physmem(int container);
int uml_vfio_user_get_iommu_info(int container, struct uml_vfio_iommu_info *info);
int uml_vfio_user_dma_map(int container, uint64_t iova, uint64_t vaddr,
			  uint64_t size);
int uml_vfio_user_dma_unmap(int container, uint64_t iova, uint64_t size);

/* @bus is the host bus the device sits on, e.g. "pci" or "platform". */
int uml_vfio_user_get_group_id(const char *bus, const char *device);
int uml_vfio_user_open_group(int group_id);
int uml_vfio_user_set_container(int container, int group);
int uml_vfio_user_unset_container(int container, int group);

int uml_vfio_user_setup_device(struct uml_vfio_user_device *dev,
			       int group, const char *device);
void uml_vfio_user_teardown_device(struct uml_vfio_user_device *dev);

int uml_vfio_user_activate_irq(struct uml_vfio_user_device *dev, int index);
void uml_vfio_user_deactivate_irq(struct uml_vfio_user_device *dev, int index);
int uml_vfio_user_update_irqs(struct uml_vfio_user_device *dev);
/* Program the device for @type, tearing down the other mode first. */
int uml_vfio_user_set_irq_type(struct uml_vfio_user_device *dev,
			       enum uml_vfio_irq_type type);

int uml_vfio_user_cfgspace_read(struct uml_vfio_user_device *dev,
				unsigned int offset, void *buf, int size);
int uml_vfio_user_cfgspace_write(struct uml_vfio_user_device *dev,
				 unsigned int offset, const void *buf, int size);

int uml_vfio_user_bar_read(struct uml_vfio_user_device *dev, int bar,
			   unsigned int offset, void *buf, int size);
int uml_vfio_user_bar_write(struct uml_vfio_user_device *dev, int bar,
			    unsigned int offset, const void *buf, int size);

/*
 * DMA address width the firmware gives @device, in bits, from the host's
 * ACPI IORT; 0 if there is no IORT or it does not limit the device. The
 * host kernel applies this as the device's bus_dma_limit, and a device
 * handed IOVAs above it faults.
 */
int uml_vfio_user_iort_dma_bits(const char *bus, const char *device);

int uml_vfio_user_platform_modalias(const char *device, char *buf, int len);
int uml_vfio_user_setup_platform(struct uml_vfio_user_platform *dev,
				 int group, const char *device);
void uml_vfio_user_teardown_platform(struct uml_vfio_user_platform *dev);
/* Returns a new eventfd the host signals interrupt @index on. */
int uml_vfio_user_platform_activate_irq(struct uml_vfio_user_platform *dev,
					int index);
void uml_vfio_user_platform_deactivate_irq(struct uml_vfio_user_platform *dev,
					   int index, int fd);
int uml_vfio_user_platform_mask_irq(struct uml_vfio_user_platform *dev,
				    int index, int mask);
int uml_vfio_user_platform_read(struct uml_vfio_user_platform *dev, int index,
				unsigned int offset, void *buf, int size);
int uml_vfio_user_platform_write(struct uml_vfio_user_platform *dev, int index,
				 unsigned int offset, const void *buf, int size);

#endif /* __UM_VFIO_USER_H */
