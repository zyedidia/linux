// SPDX-License-Identifier: GPL-2.0
/*
 * DMA mapping for VFIO devices. Each buffer is mapped into the host IOMMU
 * when the driver maps it and unmapped when the driver unmaps it, so the
 * device can only reach live DMA buffers and only those are pinned.
 */

#define pr_fmt(fmt) "vfio-uml: " fmt

#include <linux/dma-map-ops.h>
#include <linux/iova.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>

#include "vfio_dma.h"
#include "vfio_user.h"

static struct {
	int container;
	struct iova_domain iovad;
	unsigned long start_pfn;
	unsigned long limit_pfn;
	bool ready;
} uml_vfio_dma;

static unsigned long uml_vfio_dma_limit_pfn(u64 mask)
{
	return min(mask >> PAGE_SHIFT, READ_ONCE(uml_vfio_dma.limit_pfn));
}

/*
 * Allocate IOVAs below 4 GB first, as dma-iommu does for PCI devices, so that
 * a 64-bit device only uses higher addresses once that space is full.
 */
static unsigned long uml_vfio_dma_alloc_iova(unsigned long npages, u64 mask)
{
	struct iova *iova = NULL;

	if (mask > DMA_BIT_MASK(32))
		iova = alloc_iova(&uml_vfio_dma.iovad, npages,
				  uml_vfio_dma_limit_pfn(DMA_BIT_MASK(32)),
				  true);
	if (!iova)
		iova = alloc_iova(&uml_vfio_dma.iovad, npages,
				  uml_vfio_dma_limit_pfn(mask), true);

	return iova ? iova->pfn_lo : 0;
}

static dma_addr_t uml_vfio_dma_map(unsigned long vaddr, size_t size, u64 mask,
				   bool read, bool write)
{
	unsigned long offset = offset_in_page(vaddr);
	unsigned long npages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
	unsigned long pfn;
	int err;

	if (!size)
		return DMA_MAPPING_ERROR;

	pfn = uml_vfio_dma_alloc_iova(npages, mask);
	if (!pfn)
		return DMA_MAPPING_ERROR;

	err = uml_vfio_user_dma_map(uml_vfio_dma.container, vaddr - offset,
				    (u64)pfn << PAGE_SHIFT,
				    (u64)npages << PAGE_SHIFT, read, write);
	if (err) {
		free_iova(&uml_vfio_dma.iovad, pfn);
		pr_warn_ratelimited("Failed to map %zu bytes for DMA, error %d\n",
				    size, err);
		return DMA_MAPPING_ERROR;
	}

	return ((dma_addr_t)pfn << PAGE_SHIFT) + offset;
}

static void uml_vfio_dma_unmap(dma_addr_t addr, size_t size)
{
	unsigned long offset = offset_in_page(addr);
	unsigned long npages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
	u64 iova = addr - offset;
	int64_t r;

	r = uml_vfio_user_dma_unmap(uml_vfio_dma.container, iova,
				    (u64)npages << PAGE_SHIFT);

	/*
	 * After a failed or partial unmap the IOMMU state of the range is
	 * unknown, so its IOVAs are not reused.
	 */
	if (WARN(r != (int64_t)npages << PAGE_SHIFT,
		 "DMA unmap of 0x%llx (0x%lx pages) returned %lld\n",
		 iova, npages, (long long)r))
		return;

	free_iova(&uml_vfio_dma.iovad, iova >> PAGE_SHIFT);
}

static bool uml_vfio_dma_read(enum dma_data_direction dir)
{
	return dir == DMA_TO_DEVICE || dir == DMA_BIDIRECTIONAL;
}

static bool uml_vfio_dma_write(enum dma_data_direction dir)
{
	return dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL;
}

static void *uml_vfio_dma_alloc(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp,
				unsigned long attrs)
{
	void *vaddr;

	size = PAGE_ALIGN(size);
	vaddr = alloc_pages_exact(size, gfp | __GFP_ZERO);
	if (!vaddr)
		return NULL;

	*dma_handle = uml_vfio_dma_map((unsigned long)vaddr, size,
				       dev->coherent_dma_mask, true, true);
	if (*dma_handle == DMA_MAPPING_ERROR) {
		free_pages_exact(vaddr, size);
		return NULL;
	}

	return vaddr;
}

static void uml_vfio_dma_free(struct device *dev, size_t size, void *vaddr,
			      dma_addr_t dma_handle, unsigned long attrs)
{
	size = PAGE_ALIGN(size);
	uml_vfio_dma_unmap(dma_handle, size);
	free_pages_exact(vaddr, size);
}

static dma_addr_t uml_vfio_dma_map_phys(struct device *dev, phys_addr_t phys,
					size_t size,
					enum dma_data_direction dir,
					unsigned long attrs)
{
	/* BARs are not in guest memory, so peer-to-peer DMA is not supported. */
	if (attrs & DMA_ATTR_MMIO)
		return DMA_MAPPING_ERROR;

	return uml_vfio_dma_map((unsigned long)__va(phys), size,
				dma_get_mask(dev), uml_vfio_dma_read(dir),
				uml_vfio_dma_write(dir));
}

static void uml_vfio_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
				    size_t size, enum dma_data_direction dir,
				    unsigned long attrs)
{
	uml_vfio_dma_unmap(dma_handle, size);
}

static void uml_vfio_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
				  int nents, enum dma_data_direction dir,
				  unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		uml_vfio_dma_unmap(sg_dma_address(sg), sg_dma_len(sg));
}

static int uml_vfio_dma_map_sg(struct device *dev, struct scatterlist *sgl,
			       int nents, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i) {
		sg_dma_address(sg) = uml_vfio_dma_map_phys(dev, sg_phys(sg),
							   sg->length, dir,
							   attrs);
		if (sg_dma_address(sg) == DMA_MAPPING_ERROR) {
			uml_vfio_dma_unmap_sg(dev, sgl, i, dir, attrs);
			return -ENOMEM;
		}
		sg_dma_len(sg) = sg->length;
	}

	return nents;
}

static int uml_vfio_dma_supported(struct device *dev, u64 mask)
{
	return uml_vfio_dma_limit_pfn(mask) >= uml_vfio_dma.start_pfn;
}

const struct dma_map_ops uml_vfio_dma_ops = {
	.alloc = uml_vfio_dma_alloc,
	.free = uml_vfio_dma_free,
	.alloc_pages_op = dma_common_alloc_pages,
	.free_pages = dma_common_free_pages,
	.mmap = dma_common_mmap,
	.get_sgtable = dma_common_get_sgtable,
	.map_phys = uml_vfio_dma_map_phys,
	.unmap_phys = uml_vfio_dma_unmap_phys,
	.map_sg = uml_vfio_dma_map_sg,
	.unmap_sg = uml_vfio_dma_unmap_sg,
	.dma_supported = uml_vfio_dma_supported,
};

/* Convert an IOVA range to an inclusive PFN range that excludes page 0. */
static bool uml_vfio_dma_range_pfns(const struct uml_vfio_user_iova_range *r,
				    unsigned long *lo, unsigned long *hi)
{
	*lo = max(PFN_UP(r->start), 1UL);
	*hi = PFN_DOWN(min_t(u64, r->end, U64_MAX - 1) + 1);
	if (*hi <= *lo)
		return false;
	(*hi)--;
	return true;
}

/*
 * Reserve the IOVAs of the domain that are outside the given ranges, and lower
 * the limit of the domain to the end of the last range.
 */
static int uml_vfio_dma_reserve_gaps(struct uml_vfio_user_iova_range *ranges,
				     int n)
{
	unsigned long next = uml_vfio_dma.start_pfn;
	unsigned long limit = uml_vfio_dma.limit_pfn;
	unsigned long lo, hi;
	int i;

	for (i = 0; i < n && next <= limit; i++) {
		if (!uml_vfio_dma_range_pfns(&ranges[i], &lo, &hi) || hi < next)
			continue;
		if (lo > limit)
			break;
		if (lo > next &&
		    !reserve_iova(&uml_vfio_dma.iovad, next, lo - 1))
			return -ENOMEM;
		next = hi + 1;
	}

	if (next == uml_vfio_dma.start_pfn)
		return -ENOSPC;
	if (next - 1 < limit)
		WRITE_ONCE(uml_vfio_dma.limit_pfn, next - 1);
	return 0;
}

/*
 * Restrict the IOVA domain to the ranges the host IOMMU can map after a group
 * joins the container, and set the domain up if this is the first group.
 * Each group that joins can only remove ranges, so the IOVAs it excludes are
 * reserved.
 */
int uml_vfio_dma_add_group(int container)
{
	struct uml_vfio_user_iova_range *ranges;
	unsigned long lo, hi, start = 0, last = 0;
	int err, i, n;

	n = uml_vfio_user_get_iova_ranges(container, &ranges);
	if (n < 0)
		return n;

	if (!uml_vfio_dma.ready) {
		for (i = 0; i < n; i++) {
			if (!uml_vfio_dma_range_pfns(&ranges[i], &lo, &hi))
				continue;
			if (!start)
				start = lo;
			last = hi;
		}

		err = start ? iova_cache_get() : -ENOSPC;
		if (err)
			goto out;

		init_iova_domain(&uml_vfio_dma.iovad, PAGE_SIZE, start);
		uml_vfio_dma.container = container;
		uml_vfio_dma.start_pfn = start;
		uml_vfio_dma.limit_pfn = last;
		uml_vfio_dma.ready = true;
	}

	err = uml_vfio_dma_reserve_gaps(ranges, n);
out:
	kfree(ranges);
	return err;
}

/* Tear down the IOVA domain after the last group has left the container. */
void uml_vfio_dma_release(void)
{
	if (!uml_vfio_dma.ready)
		return;

	put_iova_domain(&uml_vfio_dma.iovad);
	iova_cache_put();
	uml_vfio_dma.ready = false;
}
