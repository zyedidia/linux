// SPDX-License-Identifier: GPL-2.0
/*
 * On-demand DMA mapping for VFIO-backed UML PCI devices.
 *
 * Instead of mapping (and hence pinning) all of physmem into the VFIO
 * container up front, provide dma_map_ops implemented in terms of
 * VFIO_IOMMU_MAP_DMA/VFIO_IOMMU_UNMAP_DMA so that only memory under
 * active DMA is pinned on the host.
 *
 * IOVA space is managed with an iova_domain seeded from the valid
 * ranges advertised by the host IOMMU.  Every mapping gets a freshly
 * allocated IOVA range and is created and destroyed with an exact
 * map/unmap pair, which matches the type1 rule that an unmap may not
 * split an earlier mapping.  No per-mapping state needs to be kept:
 * the IOVA range to release is recomputed from (dma_addr, size) at
 * unmap time.
 *
 * A failing MAP_DMA (e.g. RLIMIT_MEMLOCK exhaustion on the host)
 * surfaces as DMA_MAPPING_ERROR, which drivers can back off from,
 * instead of a boot-time failure.
 *
 * Host syscalls issued from these callbacks do not interact with the
 * UML scheduler, so the ioctls are safe from any context the DMA API
 * may be called in.
 */

#define pr_fmt(fmt) "vfio-uml: " fmt

#include <linux/dma-map-ops.h>
#include <linux/dma-mapping.h>
#include <linux/iova.h>
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/scatterlist.h>

#include "vfio_user.h"
#include "vfio_dma.h"

static struct iova_domain uml_vfio_iovad;
static int uml_vfio_dma_fd = -1;

/* Usable IOVA space in iova_domain granules, inclusive bounds. */
static unsigned long uml_vfio_dma_first_pfn;
static unsigned long uml_vfio_dma_last_pfn;

static dma_addr_t uml_vfio_dma_map_range(u64 mask, phys_addr_t phys,
					 size_t size)
{
	unsigned long shift = iova_shift(&uml_vfio_iovad);
	size_t off = iova_offset(&uml_vfio_iovad, phys);
	size_t len = iova_align(&uml_vfio_iovad, size + off);
	unsigned long limit_pfn, pfn;
	dma_addr_t iova;

	limit_pfn = min_t(u64, mask >> shift, uml_vfio_dma_last_pfn);

	pfn = alloc_iova_fast(&uml_vfio_iovad, len >> shift, limit_pfn, true);
	if (!pfn)
		return DMA_MAPPING_ERROR;
	iova = (dma_addr_t)pfn << shift;

	if (uml_vfio_user_dma_map(uml_vfio_dma_fd, iova,
				  (unsigned long)__va(phys - off), len)) {
		free_iova_fast(&uml_vfio_iovad, pfn, len >> shift);
		return DMA_MAPPING_ERROR;
	}

	return iova + off;
}

static void uml_vfio_dma_unmap_range(dma_addr_t addr, size_t size)
{
	unsigned long shift = iova_shift(&uml_vfio_iovad);
	size_t off = iova_offset(&uml_vfio_iovad, addr);
	size_t len = iova_align(&uml_vfio_iovad, size + off);
	dma_addr_t iova = addr - off;

	WARN_ON_ONCE(uml_vfio_user_dma_unmap(uml_vfio_dma_fd, iova, len));
	free_iova_fast(&uml_vfio_iovad, iova >> shift, len >> shift);
}

static dma_addr_t uml_vfio_dma_map_phys(struct device *dev, phys_addr_t phys,
					size_t size,
					enum dma_data_direction dir,
					unsigned long attrs)
{
	/*
	 * No P2P support: there is no translation from a BAR address
	 * to a host address we could hand to VFIO_IOMMU_MAP_DMA.
	 */
	if (attrs & DMA_ATTR_MMIO)
		return DMA_MAPPING_ERROR;

	return uml_vfio_dma_map_range(dma_get_mask(dev), phys, size);
}

static void uml_vfio_dma_unmap_phys(struct device *dev, dma_addr_t addr,
				    size_t size, enum dma_data_direction dir,
				    unsigned long attrs)
{
	if (attrs & DMA_ATTR_MMIO)
		return;

	uml_vfio_dma_unmap_range(addr, size);
}

static int uml_vfio_dma_map_sg(struct device *dev, struct scatterlist *sgl,
			       int nents, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct scatterlist *sg;
	dma_addr_t addr;
	int i, j;

	for_each_sg(sgl, sg, nents, i) {
		addr = uml_vfio_dma_map_range(dma_get_mask(dev), sg_phys(sg),
					      sg->length);
		if (addr == DMA_MAPPING_ERROR)
			goto unwind;
		sg->dma_address = addr;
		sg_dma_len(sg) = sg->length;
	}

	return nents;

unwind:
	for_each_sg(sgl, sg, i, j)
		uml_vfio_dma_unmap_range(sg_dma_address(sg), sg_dma_len(sg));
	return -ENOMEM;
}

static void uml_vfio_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
				  int nents, enum dma_data_direction dir,
				  unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		uml_vfio_dma_unmap_range(sg_dma_address(sg), sg_dma_len(sg));
}

static void *uml_vfio_dma_alloc(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp,
				unsigned long attrs)
{
	void *vaddr;
	dma_addr_t addr;

	size = PAGE_ALIGN(size);
	vaddr = alloc_pages_exact(size, gfp | __GFP_ZERO);
	if (!vaddr)
		return NULL;

	addr = uml_vfio_dma_map_range(dev->coherent_dma_mask, __pa(vaddr),
				      size);
	if (addr == DMA_MAPPING_ERROR) {
		free_pages_exact(vaddr, size);
		return NULL;
	}

	*dma_handle = addr;
	return vaddr;
}

static void uml_vfio_dma_free(struct device *dev, size_t size, void *vaddr,
			      dma_addr_t dma_handle, unsigned long attrs)
{
	size = PAGE_ALIGN(size);
	uml_vfio_dma_unmap_range(dma_handle, size);
	free_pages_exact(vaddr, size);
}

static struct page *uml_vfio_dma_alloc_pages(struct device *dev, size_t size,
					     dma_addr_t *dma_handle,
					     enum dma_data_direction dir,
					     gfp_t gfp)
{
	struct page *page;
	dma_addr_t addr;

	page = alloc_pages(gfp | __GFP_ZERO, get_order(size));
	if (!page)
		return NULL;

	addr = uml_vfio_dma_map_range(dev->coherent_dma_mask,
				      page_to_phys(page), size);
	if (addr == DMA_MAPPING_ERROR) {
		__free_pages(page, get_order(size));
		return NULL;
	}

	*dma_handle = addr;
	return page;
}

static void uml_vfio_dma_free_pages(struct device *dev, size_t size,
				    struct page *page, dma_addr_t dma_handle,
				    enum dma_data_direction dir)
{
	uml_vfio_dma_unmap_range(dma_handle, size);
	__free_pages(page, get_order(size));
}

static int uml_vfio_dma_supported(struct device *dev, u64 mask)
{
	/* There must be some usable IOVA space below the mask. */
	return (mask >> iova_shift(&uml_vfio_iovad)) > uml_vfio_dma_first_pfn;
}

static u64 uml_vfio_dma_get_required_mask(struct device *dev)
{
	u64 top = ((u64)(uml_vfio_dma_last_pfn + 1)
		   << iova_shift(&uml_vfio_iovad)) - 1;

	return DMA_BIT_MASK(fls64(top));
}

const struct dma_map_ops uml_vfio_dma_ops = {
	.alloc			= uml_vfio_dma_alloc,
	.free			= uml_vfio_dma_free,
	.alloc_pages_op		= uml_vfio_dma_alloc_pages,
	.free_pages		= uml_vfio_dma_free_pages,
	.mmap			= dma_common_mmap,
	.get_sgtable		= dma_common_get_sgtable,
	.map_phys		= uml_vfio_dma_map_phys,
	.unmap_phys		= uml_vfio_dma_unmap_phys,
	.map_sg			= uml_vfio_dma_map_sg,
	.unmap_sg		= uml_vfio_dma_unmap_sg,
	.dma_supported		= uml_vfio_dma_supported,
	.get_required_mask	= uml_vfio_dma_get_required_mask,
};

static int uml_vfio_dma_setup_iovad(const struct uml_vfio_iommu_info *info)
{
	struct uml_vfio_iova_range fallback = {
		.start = 0,
		.end = DMA_BIT_MASK(63),
	};
	const struct uml_vfio_iova_range *ranges = info->ranges;
	int nranges = info->nranges;
	unsigned long granule, shift;
	unsigned long *lo, *hi;
	int i, j, n, err;

	/*
	 * The domain granule is the host IOMMU's smallest page size:
	 * MAP_DMA requires iova/vaddr/size aligned to it.  It may be
	 * smaller than PAGE_SIZE (pinning is then tighter than a UML
	 * page) or, on exotic hosts, larger (mappings are then aligned
	 * out, over-pinning the neighbouring memory).
	 */
	granule = PAGE_SIZE;
	if (info->pgsizes)
		granule = 1UL << __ffs64(info->pgsizes);
	if (granule > PAGE_SIZE)
		pr_info("host IOMMU granule %#lx > page size, DMA maps will be aligned up\n",
			granule);
	shift = __ffs(granule);

	if (nranges <= 0) {
		pr_warn("host did not advertise IOVA ranges, assuming a full 63-bit space\n");
		ranges = &fallback;
		nranges = 1;
	}

	lo = kmalloc_array(nranges, 2 * sizeof(*lo), GFP_KERNEL);
	if (!lo)
		return -ENOMEM;
	hi = lo + nranges;

	/* Convert to inclusive bounds of fully-usable granules. */
	for (i = 0, n = 0; i < nranges; i++) {
		u64 pfn_lo, pfn_hi;

		if (ranges[i].start > ranges[i].end)
			continue;

		pfn_lo = (ranges[i].start + granule - 1) >> shift;
		pfn_hi = ranges[i].end >> shift;
		if ((ranges[i].end & (granule - 1)) != granule - 1) {
			if (!pfn_hi)
				continue;
			pfn_hi--;
		}

		/* Never hand out IOVA 0. */
		if (!pfn_lo)
			pfn_lo = 1;
		if (pfn_lo > pfn_hi)
			continue;

		lo[n] = pfn_lo;
		hi[n] = pfn_hi;
		n++;
	}

	if (!n) {
		pr_err("no usable IOVA range\n");
		err = -ENODEV;
		goto free;
	}

	/* Sort ascending; n is tiny. */
	for (i = 1; i < n; i++) {
		for (j = i; j > 0 && lo[j - 1] > lo[j]; j--) {
			swap(lo[j - 1], lo[j]);
			swap(hi[j - 1], hi[j]);
		}
	}

	init_iova_domain(&uml_vfio_iovad, granule, lo[0]);
	err = iova_domain_init_rcaches(&uml_vfio_iovad);
	if (err)
		goto free;

	/* Punch out the holes between usable ranges. */
	for (i = 0; i < n - 1; i++) {
		if (lo[i + 1] <= hi[i] + 1)
			continue;
		if (!reserve_iova(&uml_vfio_iovad, hi[i] + 1, lo[i + 1] - 1)) {
			err = -ENOMEM;
			goto put_domain;
		}
	}

	uml_vfio_dma_first_pfn = lo[0];
	uml_vfio_dma_last_pfn = hi[n - 1];

	pr_info("on-demand DMA: granule %#lx, IOVA space [%#llx, %#llx], %d range(s)\n",
		granule, (u64)lo[0] << shift,
		(((u64)hi[n - 1] + 1) << shift) - 1, n);

	kfree(lo);
	return 0;

put_domain:
	put_iova_domain(&uml_vfio_iovad);
free:
	kfree(lo);
	return err;
}

int uml_vfio_dma_init(int container_fd)
{
	struct uml_vfio_iommu_info info;
	int err;

	err = iova_cache_get();
	if (err)
		return err;

	err = uml_vfio_user_get_iommu_info(container_fd, &info);
	if (err)
		goto put_cache;

	err = uml_vfio_dma_setup_iovad(&info);
	kfree(info.ranges);
	if (err)
		goto put_cache;

	uml_vfio_dma_fd = container_fd;
	return 0;

put_cache:
	iova_cache_put();
	return err;
}

void uml_vfio_dma_exit(void)
{
	if (uml_vfio_dma_fd < 0)
		return;

	put_iova_domain(&uml_vfio_iovad);
	iova_cache_put();
	uml_vfio_dma_fd = -1;
}
