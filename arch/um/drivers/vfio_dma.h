/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VFIO_DMA_H
#define __UM_VFIO_DMA_H

struct dma_map_ops;

extern const struct dma_map_ops uml_vfio_dma_ops;

int uml_vfio_dma_init(int container_fd);
void uml_vfio_dma_exit(void);

#endif /* __UM_VFIO_DMA_H */
