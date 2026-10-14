/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VFIO_KERN_H
#define __UM_VFIO_KERN_H

/*
 * Shared between the PCI (vfio_kern.c) and platform (vfio_platform.c)
 * halves of vfio_uml: both put their groups in the one container, so
 * the IOVA space and on-demand DMA mapping are common.
 */

extern bool uml_vfio_eager_dma;

int uml_vfio_open_container(void);
int uml_vfio_open_group(int group_id);
int uml_vfio_release_group(int group_fd);
int uml_vfio_dma_ready(void);

#ifdef CONFIG_UML_PLATFORM_OVER_VFIO
void uml_vfio_platform_init(void);
void uml_vfio_platform_exit(void);
#else
static inline void uml_vfio_platform_init(void) {}
static inline void uml_vfio_platform_exit(void) {}
#endif

#endif /* __UM_VFIO_KERN_H */
