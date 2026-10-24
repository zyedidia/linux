/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VFIO_USER_H
#define __UM_VFIO_USER_H

struct uml_vfio_user_range {
	uint64_t offset;
	uint64_t size;
};

/* An inclusive range of IOVAs that the host IOMMU can map. */
struct uml_vfio_user_iova_range {
	uint64_t start;
	uint64_t end;
};

enum {
	UML_VFIO_IRQ_NONE,
	UML_VFIO_IRQ_MSI,
	UML_VFIO_IRQ_MSIX,
};

struct uml_vfio_user_device {
	int device;
	/* Set if VFIO can reset the device. */
	int can_reset;

	struct {
		uint64_t size;
		uint64_t offset;
	} *region;
	int num_regions;

	/* Eventfds of the vectors, shared by MSI and MSI-X. */
	int32_t *irqfd;
	int irq_count;
	int msi_count;
	int msix_count;
	/* Buffer for setting the eventfds of every vector. */
	void *irq_set;
};

int uml_vfio_user_check_container(int fd);
int uml_vfio_user_open_container(void);
int uml_vfio_user_setup_iommu(int container);
int uml_vfio_user_get_iova_ranges(int container,
				  struct uml_vfio_user_iova_range **ranges);
int uml_vfio_user_dma_map(int container, unsigned long vaddr, uint64_t iova,
			  uint64_t size, int read, int write);
int64_t uml_vfio_user_dma_unmap(int container, uint64_t iova, uint64_t size);

int uml_vfio_user_get_group_id(const char *device);
int uml_vfio_user_open_group(int group_id);
int uml_vfio_user_set_container(int container, int group);
int uml_vfio_user_unset_container(int container, int group);

int uml_vfio_user_setup_device(struct uml_vfio_user_device *dev,
			       int group, const char *device);
void uml_vfio_user_teardown_device(struct uml_vfio_user_device *dev);
int uml_vfio_user_reset(struct uml_vfio_user_device *dev);

int uml_vfio_user_activate_irq(struct uml_vfio_user_device *dev, int index);
void uml_vfio_user_deactivate_irq(struct uml_vfio_user_device *dev, int index);
int uml_vfio_user_enable_irqs(struct uml_vfio_user_device *dev, int mode);
int uml_vfio_user_update_irq(struct uml_vfio_user_device *dev, int mode,
			     int index);
int uml_vfio_user_disable_irqs(struct uml_vfio_user_device *dev, int mode);

int uml_vfio_user_cfgspace_read(struct uml_vfio_user_device *dev,
				unsigned int offset, void *buf, int size);
int uml_vfio_user_cfgspace_write(struct uml_vfio_user_device *dev,
				 unsigned int offset, const void *buf, int size);

int uml_vfio_user_bar_read(struct uml_vfio_user_device *dev, int bar,
			   unsigned int offset, void *buf, int size);
int uml_vfio_user_bar_write(struct uml_vfio_user_device *dev, int bar,
			    unsigned int offset, const void *buf, int size);

void *uml_vfio_user_map_bar(struct uml_vfio_user_device *dev, int bar,
			    const struct uml_vfio_user_range *shadow,
			    int nr_shadow);
void uml_vfio_user_unmap_bar(struct uml_vfio_user_device *dev, int bar,
			     void *addr);

#endif /* __UM_VFIO_USER_H */
