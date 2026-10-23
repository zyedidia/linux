/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VFIO_DMA_H
#define __UM_VFIO_DMA_H

#include <linux/dma-map-ops.h>

extern const struct dma_map_ops uml_vfio_dma_ops;

int uml_vfio_dma_add_group(int container);
void uml_vfio_dma_release(void);

#endif /* __UM_VFIO_DMA_H */
