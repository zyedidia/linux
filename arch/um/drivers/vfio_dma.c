// SPDX-License-Identifier: GPL-2.0
/*
 * DMA mapping for VFIO devices. Each buffer is mapped into the host IOMMU
 * when the driver maps it and unmapped when the driver unmaps it, so the
 * device can only reach live DMA buffers and only those are pinned. With
 * vfio_uml.eager_dma=1, the guest's RAM is instead mapped once, when the
 * container is set up, and a buffer in it needs no map of its own. All of
 * RAM is then pinned and reachable by the device. With
 * vfio_uml.dma_cache_mb=<n>, a small buffer's mapping instead stays mapped
 * after its unmap, up to n MiB of unused mappings, so that the next buffer
 * in the same pages needs no map of its own.
 */

#define pr_fmt(fmt) "vfio-uml: " fmt

#include <linux/dma-map-ops.h>
#include <linux/hashtable.h>
#include <linux/iova.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/scatterlist.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <as-layout.h>
#include <kern_util.h>

#include "vfio_dma.h"
#include "vfio_user.h"

static bool uml_vfio_eager_dma;
module_param_named(eager_dma, uml_vfio_eager_dma, bool, 0400);

static unsigned int uml_vfio_dma_cache_mb;
module_param_named(dma_cache_mb, uml_vfio_dma_cache_mb, uint, 0400);

#define UML_VFIO_DMA_CACHE_PAGES 16
#define UML_VFIO_DMA_CACHE_BITS 12

/*
 * A cached mapping of npages pages from vstart at iova, for reading and
 * writing. Refs counts the buffers that use it. An unused mapping stays
 * mapped on the LRU list until it is evicted. A stale mapping, whose pages
 * are leaving the host file, is never used again and is unmapped once it is
 * unused.
 */
struct uml_vfio_dma_entry {
	struct hlist_node by_vaddr;
	struct hlist_node by_iova;
	struct list_head lru;
	unsigned long vstart;
	unsigned long npages;
	dma_addr_t iova;
	unsigned int refs;
	bool stale;
};

/*
 * The lock protects the entries, the tables, the LRU list and unused_pages.
 * The mutex keeps a release from tearing the domain down while an
 * invalidation unmaps the entries that it removed.
 */
static struct {
	spinlock_t lock;
	struct mutex mutex;
	DECLARE_HASHTABLE(by_vaddr, UML_VFIO_DMA_CACHE_BITS);
	DECLARE_HASHTABLE(by_iova, UML_VFIO_DMA_CACHE_BITS);
	struct list_head lru;
	unsigned long unused_pages;
} uml_vfio_dma_cache = {
	.lock = __SPIN_LOCK_UNLOCKED(uml_vfio_dma_cache.lock),
	.mutex = __MUTEX_INITIALIZER(uml_vfio_dma_cache.mutex),
	.lru = LIST_HEAD_INIT(uml_vfio_dma_cache.lru),
};

/*
 * With eager DMA, the RAM from ram_start to ram_end is mapped at ram_iova
 * while eager is set, which is published after the range.
 */
static struct {
	int container;
	struct iova_domain iovad;
	unsigned long start_pfn;
	unsigned long limit_pfn;
	bool ready;
	bool eager;
	unsigned long ram_start;
	unsigned long ram_end;
	dma_addr_t ram_iova;
} uml_vfio_dma;

/*
 * Counters for the container. Byte counts are of whole pages, which is what
 * the host pins. Premapped counts the maps that the eagerly mapped RAM
 * served, and cached those that a cached mapping served.
 */
static struct {
	atomic64_t premapped;
	atomic64_t cached;
	atomic64_t maps;
	atomic64_t unmaps;
	atomic64_t map_errors;
	atomic64_t unmap_errors;
	atomic64_t mapped_bytes;
	atomic64_t live_bytes;
	atomic64_t peak_bytes;
} uml_vfio_dma_stats;

static void uml_vfio_dma_count_map(unsigned long npages)
{
	s64 bytes = (s64)npages << PAGE_SHIFT;
	s64 live, peak;

	atomic64_inc(&uml_vfio_dma_stats.maps);
	atomic64_add(bytes, &uml_vfio_dma_stats.mapped_bytes);
	live = atomic64_add_return(bytes, &uml_vfio_dma_stats.live_bytes);

	peak = atomic64_read(&uml_vfio_dma_stats.peak_bytes);
	while (live > peak &&
	       !atomic64_try_cmpxchg(&uml_vfio_dma_stats.peak_bytes, &peak,
				     live))
		;
}

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

/*
 * Return the IOVA of a buffer in the eagerly mapped RAM, if the device's mask
 * reaches it, or 0, which is never an IOVA.
 */
static dma_addr_t uml_vfio_dma_ram_addr(unsigned long vaddr, size_t size,
					u64 mask)
{
	dma_addr_t addr;

	if (!smp_load_acquire(&uml_vfio_dma.eager) ||
	    vaddr < uml_vfio_dma.ram_start ||
	    vaddr >= uml_vfio_dma.ram_end ||
	    size > uml_vfio_dma.ram_end - vaddr)
		return 0;
	addr = uml_vfio_dma.ram_iova + (vaddr - uml_vfio_dma.ram_start);
	return addr + size - 1 <= mask ? addr : 0;
}

static bool uml_vfio_dma_in_ram(dma_addr_t addr)
{
	return smp_load_acquire(&uml_vfio_dma.eager) &&
	       addr >= uml_vfio_dma.ram_iova &&
	       addr - uml_vfio_dma.ram_iova <
	       uml_vfio_dma.ram_end - uml_vfio_dma.ram_start;
}

static dma_addr_t uml_vfio_dma_map(unsigned long vaddr, size_t size, u64 mask,
				   bool read, bool write)
{
	unsigned long offset = offset_in_page(vaddr);
	unsigned long npages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
	unsigned long pfn;
	dma_addr_t addr;
	int err;

	if (!size)
		return DMA_MAPPING_ERROR;

	addr = uml_vfio_dma_ram_addr(vaddr, size, mask);
	if (addr) {
		atomic64_inc(&uml_vfio_dma_stats.premapped);
		return addr;
	}

	pfn = uml_vfio_dma_alloc_iova(npages, mask);
	if (!pfn) {
		atomic64_inc(&uml_vfio_dma_stats.map_errors);
		return DMA_MAPPING_ERROR;
	}

	err = uml_vfio_user_dma_map(uml_vfio_dma.container, vaddr - offset,
				    (u64)pfn << PAGE_SHIFT,
				    (u64)npages << PAGE_SHIFT, read, write);
	if (err) {
		free_iova(&uml_vfio_dma.iovad, pfn);
		atomic64_inc(&uml_vfio_dma_stats.map_errors);
		pr_warn_ratelimited("Failed to map %zu bytes for DMA, error %d\n",
				    size, err);
		return DMA_MAPPING_ERROR;
	}

	uml_vfio_dma_count_map(npages);
	return ((dma_addr_t)pfn << PAGE_SHIFT) + offset;
}

static void uml_vfio_dma_unmap(dma_addr_t addr, size_t size)
{
	unsigned long offset = offset_in_page(addr);
	unsigned long npages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
	u64 iova = addr - offset;
	int64_t r;

	if (uml_vfio_dma_in_ram(addr))
		return;

	r = uml_vfio_user_dma_unmap(uml_vfio_dma.container, iova,
				    (u64)npages << PAGE_SHIFT);

	/*
	 * After a failed or partial unmap the IOMMU state of the range is
	 * unknown, so its IOVAs are not reused.
	 */
	if (WARN(r != (int64_t)npages << PAGE_SHIFT,
		 "DMA unmap of 0x%llx (0x%lx pages) returned %lld\n",
		 iova, npages, (long long)r)) {
		atomic64_inc(&uml_vfio_dma_stats.unmap_errors);
		return;
	}

	free_iova(&uml_vfio_dma.iovad, iova >> PAGE_SHIFT);
	atomic64_inc(&uml_vfio_dma_stats.unmaps);
	atomic64_sub((s64)npages << PAGE_SHIFT,
		     &uml_vfio_dma_stats.live_bytes);
}

/*
 * Move an entry from the tables to a list of entries to unmap, with the lock
 * held. An unused entry that is not stale also leaves the LRU list.
 */
static void uml_vfio_dma_cache_remove(struct uml_vfio_dma_entry *e,
				      struct list_head *unmap)
{
	hash_del(&e->by_vaddr);
	hash_del(&e->by_iova);
	if (!e->refs && !e->stale) {
		list_del(&e->lru);
		uml_vfio_dma_cache.unused_pages -= e->npages;
	}
	list_add(&e->lru, unmap);
}

static void uml_vfio_dma_cache_unmap(struct list_head *unmap)
{
	struct uml_vfio_dma_entry *e, *tmp;

	list_for_each_entry_safe(e, tmp, unmap, lru) {
		uml_vfio_dma_unmap(e->iova, e->npages << PAGE_SHIFT);
		kfree(e);
	}
}

/*
 * Map a streaming buffer through the cache. A buffer whose first page starts
 * a cached mapping that covers it, and that the device's mask reaches, uses
 * that mapping. Otherwise its pages get a new cached mapping, for reading
 * and writing. Only small buffers in the RAM of the host file are cached,
 * which is all that can be dropped from it. Other buffers are mapped as
 * before, with the direction's permissions, since the kernel image's
 * read-only data cannot be mapped for writing. With eager DMA that includes
 * the buffers that the mapped RAM serves.
 */
static dma_addr_t uml_vfio_dma_map_cached(unsigned long vaddr, size_t size,
					  u64 mask, bool read, bool write)
{
	unsigned long offset = offset_in_page(vaddr);
	unsigned long vstart = vaddr - offset;
	unsigned long npages = PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
	struct uml_vfio_dma_entry *e;
	unsigned long flags;
	dma_addr_t iova;

	if (!uml_vfio_dma_cache_mb || !size ||
	    npages > UML_VFIO_DMA_CACHE_PAGES ||
	    vstart < PAGE_ALIGN(uml_reserved) || vstart >= high_physmem ||
	    npages > (high_physmem - vstart) >> PAGE_SHIFT ||
	    smp_load_acquire(&uml_vfio_dma.eager))
		return uml_vfio_dma_map(vaddr, size, mask, read, write);

	spin_lock_irqsave(&uml_vfio_dma_cache.lock, flags);
	hash_for_each_possible(uml_vfio_dma_cache.by_vaddr, e, by_vaddr,
			       vstart) {
		if (e->vstart != vstart || e->npages < npages || e->stale ||
		    e->iova + offset + size - 1 > mask)
			continue;
		if (!e->refs++) {
			list_del(&e->lru);
			uml_vfio_dma_cache.unused_pages -= e->npages;
		}
		iova = e->iova;
		spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);
		atomic64_inc(&uml_vfio_dma_stats.cached);
		return iova + offset;
	}
	spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);

	e = kmalloc(sizeof(*e), GFP_ATOMIC);
	if (!e)
		return uml_vfio_dma_map(vaddr, size, mask, read, write);
	iova = uml_vfio_dma_map(vstart, npages << PAGE_SHIFT, mask, true, true);
	if (iova == DMA_MAPPING_ERROR) {
		kfree(e);
		return iova;
	}
	e->vstart = vstart;
	e->npages = npages;
	e->iova = iova;
	e->refs = 1;
	e->stale = false;
	spin_lock_irqsave(&uml_vfio_dma_cache.lock, flags);
	hash_add(uml_vfio_dma_cache.by_vaddr, &e->by_vaddr, vstart);
	hash_add(uml_vfio_dma_cache.by_iova, &e->by_iova, iova);
	spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);
	return iova + offset;
}

/*
 * Release a buffer's cached mapping, which stays mapped while it is unused,
 * evicting the least recently used ones beyond the cache's size. A buffer
 * without a cached mapping is unmapped.
 */
static void uml_vfio_dma_unmap_cached(dma_addr_t addr, size_t size)
{
	unsigned long limit = (unsigned long)uml_vfio_dma_cache_mb <<
			      (20 - PAGE_SHIFT);
	dma_addr_t iova = addr & PAGE_MASK;
	struct uml_vfio_dma_entry *e;
	unsigned long flags;
	LIST_HEAD(unmap);

	/* No entries exist without the cache, or for the eagerly mapped RAM. */
	if (!uml_vfio_dma_cache_mb || uml_vfio_dma_in_ram(addr)) {
		uml_vfio_dma_unmap(addr, size);
		return;
	}

	spin_lock_irqsave(&uml_vfio_dma_cache.lock, flags);
	hash_for_each_possible(uml_vfio_dma_cache.by_iova, e, by_iova, iova) {
		if (e->iova != iova)
			continue;
		if (WARN_ON_ONCE(!e->refs) || --e->refs)
			break;
		if (e->stale) {
			uml_vfio_dma_cache_remove(e, &unmap);
			break;
		}
		list_add(&e->lru, &uml_vfio_dma_cache.lru);
		uml_vfio_dma_cache.unused_pages += e->npages;
		while (uml_vfio_dma_cache.unused_pages > limit)
			uml_vfio_dma_cache_remove(list_last_entry(&uml_vfio_dma_cache.lru,
								  struct uml_vfio_dma_entry,
								  lru),
						  &unmap);
		break;
	}
	spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);

	if (!e)
		uml_vfio_dma_unmap(addr, size);
	uml_vfio_dma_cache_unmap(&unmap);
}

/*
 * Before pages of RAM leave the host file, the cached mappings that include
 * them are unmapped, or made stale while a buffer uses them, since the IOMMU
 * would keep the old pages. A mapping that includes a page starts at most
 * UML_VFIO_DMA_CACHE_PAGES - 1 pages before it.
 */
void uml_vfio_dma_invalidate(unsigned long start, unsigned long len)
{
	unsigned long span = (UML_VFIO_DMA_CACHE_PAGES - 1) << PAGE_SHIFT;
	unsigned long first = start & PAGE_MASK, end = start + len, v;
	struct uml_vfio_dma_entry *e;
	struct hlist_node *tmp;
	unsigned long flags;
	LIST_HEAD(unmap);

	if (!uml_vfio_dma_cache_mb || !len)
		return;
	first = first > span ? first - span : 0;
	guard(mutex)(&uml_vfio_dma_cache.mutex);
	spin_lock_irqsave(&uml_vfio_dma_cache.lock, flags);
	for (v = first; v < end; v += PAGE_SIZE) {
		hash_for_each_possible_safe(uml_vfio_dma_cache.by_vaddr, e, tmp,
					    by_vaddr, v) {
			if (e->vstart != v ||
			    v + (e->npages << PAGE_SHIFT) <= start)
				continue;
			if (e->refs)
				e->stale = true;
			else
				uml_vfio_dma_cache_remove(e, &unmap);
		}
	}
	spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);
	uml_vfio_dma_cache_unmap(&unmap);
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

	return uml_vfio_dma_map_cached((unsigned long)__va(phys), size,
				       dma_get_mask(dev),
				       uml_vfio_dma_read(dir),
				       uml_vfio_dma_write(dir));
}

static void uml_vfio_dma_unmap_phys(struct device *dev, dma_addr_t dma_handle,
				    size_t size, enum dma_data_direction dir,
				    unsigned long attrs)
{
	uml_vfio_dma_unmap_cached(dma_handle, size);
}

static void uml_vfio_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
				  int nents, enum dma_data_direction dir,
				  unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		uml_vfio_dma_unmap_cached(sg_dma_address(sg), sg_dma_len(sg));
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
 * Map the RAM that the page allocator manages, from the end of the kernel
 * image to the end of physical memory, at one IOVA range. If that fails,
 * each buffer is mapped on its own. Free pages stop being returned to the
 * host before the mapping, which would keep the old pages of any that are.
 */
static void uml_vfio_dma_map_ram(void)
{
	unsigned long start = PAGE_ALIGN(uml_reserved);
	unsigned long end = high_physmem & PAGE_MASK;
	unsigned long npages = (end - start) >> PAGE_SHIFT;
	unsigned long pfn;
	int err;

	if (IS_ENABLED(CONFIG_UML_PAGE_REPORTING))
		uml_page_reporting_stop();
	uml_pin_ram();

	pfn = uml_vfio_dma_alloc_iova(npages, DMA_BIT_MASK(64));
	if (!pfn) {
		pr_warn("No IOVA range for %lu MiB of RAM, mapping each buffer\n",
			(end - start) >> 20);
		return;
	}
	err = uml_vfio_user_dma_map(uml_vfio_dma.container, start,
				    (u64)pfn << PAGE_SHIFT,
				    (u64)npages << PAGE_SHIFT, true, true);
	if (err) {
		free_iova(&uml_vfio_dma.iovad, pfn);
		pr_warn("Failed to map %lu MiB of RAM, error %d, mapping each buffer\n",
			(end - start) >> 20, err);
		return;
	}
	uml_vfio_dma_count_map(npages);
	uml_vfio_dma.ram_start = start;
	uml_vfio_dma.ram_end = end;
	uml_vfio_dma.ram_iova = (dma_addr_t)pfn << PAGE_SHIFT;
	smp_store_release(&uml_vfio_dma.eager, true);
	pr_info("Mapped %lu MiB of RAM for DMA at IOVA 0x%llx\n",
		(end - start) >> 20, (u64)uml_vfio_dma.ram_iova);
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
	if (!err && uml_vfio_eager_dma && !uml_vfio_dma.eager)
		uml_vfio_dma_map_ram();
out:
	kfree(ranges);
	return err;
}

void uml_vfio_dma_show(struct seq_file *m)
{
	seq_puts(m, "dma\n");
	seq_printf(m, "  %-18s %lld\n", "premapped",
		   atomic64_read(&uml_vfio_dma_stats.premapped));
	seq_printf(m, "  %-18s %lld\n", "cached",
		   atomic64_read(&uml_vfio_dma_stats.cached));
	seq_printf(m, "  %-18s %lu\n", "cache_unused_bytes",
		   READ_ONCE(uml_vfio_dma_cache.unused_pages) << PAGE_SHIFT);
	seq_printf(m, "  %-18s %lld\n", "maps",
		   atomic64_read(&uml_vfio_dma_stats.maps));
	seq_printf(m, "  %-18s %lld\n", "unmaps",
		   atomic64_read(&uml_vfio_dma_stats.unmaps));
	seq_printf(m, "  %-18s %lld\n", "map_errors",
		   atomic64_read(&uml_vfio_dma_stats.map_errors));
	seq_printf(m, "  %-18s %lld\n", "unmap_errors",
		   atomic64_read(&uml_vfio_dma_stats.unmap_errors));
	seq_printf(m, "  %-18s %lld\n", "mapped_bytes",
		   atomic64_read(&uml_vfio_dma_stats.mapped_bytes));
	seq_printf(m, "  %-18s %lld\n", "live_bytes",
		   atomic64_read(&uml_vfio_dma_stats.live_bytes));
	seq_printf(m, "  %-18s %lld\n", "peak_live_bytes",
		   atomic64_read(&uml_vfio_dma_stats.peak_bytes));
}

/*
 * Tear down the IOVA domain and forget the cached mappings when the last
 * group leaves the container, which then drops the host's mappings. An
 * invalidation that is still unmapping finishes first.
 */
void uml_vfio_dma_release(void)
{
	struct uml_vfio_dma_entry *e;
	struct hlist_node *tmp;
	unsigned long flags;
	int bkt;

	if (!uml_vfio_dma.ready)
		return;

	guard(mutex)(&uml_vfio_dma_cache.mutex);
	spin_lock_irqsave(&uml_vfio_dma_cache.lock, flags);
	hash_for_each_safe(uml_vfio_dma_cache.by_vaddr, bkt, tmp, e, by_vaddr) {
		hash_del(&e->by_vaddr);
		hash_del(&e->by_iova);
		atomic64_sub((s64)e->npages << PAGE_SHIFT,
			     &uml_vfio_dma_stats.live_bytes);
		kfree(e);
	}
	INIT_LIST_HEAD(&uml_vfio_dma_cache.lru);
	uml_vfio_dma_cache.unused_pages = 0;
	spin_unlock_irqrestore(&uml_vfio_dma_cache.lock, flags);

	if (uml_vfio_dma.eager)
		atomic64_sub(uml_vfio_dma.ram_end - uml_vfio_dma.ram_start,
			     &uml_vfio_dma_stats.live_bytes);
	put_iova_domain(&uml_vfio_dma.iovad);
	iova_cache_put();
	WRITE_ONCE(uml_vfio_dma.eager, false);
	uml_vfio_dma.ready = false;
}
