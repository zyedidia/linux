// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-guest-gpu: " fmt

#include <drm/amdgpu_drm.h>
#include <drm/drm_gem.h>
#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/uio.h>
#include <linux/vduse_guest.h>
#include <linux/vmalloc.h>
#include <linux/xarray.h>
#include <uapi/linux/virtio_gpu.h>
#include <uapi/linux/virtio_ids.h>

#include "gpu.h"

#define VDUSE_GUEST_GPU_CTRLQ 0
#define VDUSE_GUEST_GPU_CURSORQ 1
#define VDUSE_GUEST_GPU_INLINE_IOVS 4
#define VDUSE_GUEST_GPU_RENDER "/dev/dri/renderD128"
#define VDUSE_GUEST_GPU_REGION_SIZE SZ_8G
#define VDUSE_GUEST_GPU_SHMEM_MAX SZ_16M
#define VDUSE_GUEST_GPU_MAP_BATCH 65536UL

/*
 * Each context holds a DRM file and an amdgpu VM, and each shared memory
 * buffer guest pages that no limit of the GPU accounts, so the device
 * bounds both.
 */
#define VDUSE_GUEST_GPU_MAX_CONTEXTS 256
#define VDUSE_GUEST_GPU_SHMEM_TOTAL SZ_64M

/* The largest ccmd stream of a SUBMIT_3D. */
#define VDUSE_GUEST_GPU_SUBMIT_MAX SZ_1M

/*
 * The worker's memory, which holds the arguments of the ioctls that it
 * runs, including a CS_SUBMIT's translated chunks.
 */
#define VDUSE_GUEST_GPU_UMEM_SIZE SZ_4M

/*
 * Every fenced SUBMIT_3D holds its descriptors until the GPU finishes it, so
 * the control queue is large.
 */
static const u16 vduse_guest_gpu_queue_sizes[] = {
	[VDUSE_GUEST_GPU_CTRLQ] = 1024,
	[VDUSE_GUEST_GPU_CURSORQ] = 16,
};

/* A fenced command whose completion waits for a fence. */
struct vduse_guest_gpu_fence {
	struct vduse_guest_gpu *gpu;
	struct dma_fence *fence;
	struct dma_fence_cb cb;
	struct list_head list;
	u16 head;
	u32 len;
};

/* The commands that the backend reads into memory, without their data. */
union vduse_guest_gpu_req {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_get_capset_info get_capset_info;
	struct virtio_gpu_get_capset get_capset;
	struct virtio_gpu_ctx_create ctx_create;
	struct virtio_gpu_ctx_destroy ctx_destroy;
	struct virtio_gpu_ctx_resource ctx_resource;
	struct virtio_gpu_resource_create_blob create_blob;
	struct virtio_gpu_resource_map_blob map_blob;
	struct virtio_gpu_resource_unmap_blob unmap_blob;
	struct virtio_gpu_resource_unref unref;
	struct virtio_gpu_cmd_submit submit;
	struct virtio_gpu_set_scanout set_scanout;
	struct virtio_gpu_set_scanout_blob set_scanout_blob;
	struct virtio_gpu_resource_flush flush;
	struct virtio_gpu_cmd_get_edid get_edid;
};

/* Take size bytes, 8-aligned, of the worker's memory. */
void __user *vduse_guest_gpu_ualloc(struct vduse_guest_gpu *gpu, size_t size)
{
	void __user *p;

	size = ALIGN(size, 8);
	if (size > VDUSE_GUEST_GPU_UMEM_SIZE - gpu->umem_used)
		return NULL;
	p = gpu->umem + gpu->umem_used;
	gpu->umem_used += size;
	return p;
}

/* Release all of the worker's memory for the next ioctls. */
void vduse_guest_gpu_ureset(struct vduse_guest_gpu *gpu)
{
	gpu->umem_used = 0;
}

/*
 * Give the worker an mm of its own, with memory for the arguments of the
 * ioctls that it runs. The ioctls read and write their arguments with the
 * user copy functions, which reach the mm while the worker adopts it.
 */
static int vduse_guest_gpu_init_mm(struct vduse_guest_gpu *gpu)
{
	struct mm_struct *mm;
	unsigned long addr;

	if (gpu->mm)
		return 0;

	mm = mm_alloc();
	if (!mm)
		return -ENOMEM;
	mm->task_size = TASK_SIZE;
	arch_pick_mmap_layout(mm, &current->signal->rlim[RLIMIT_STACK]);

	kthread_use_mm(mm);
	addr = vm_mmap(NULL, 0, VDUSE_GUEST_GPU_UMEM_SIZE,
		       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0);
	kthread_unuse_mm(mm);
	if (IS_ERR_VALUE(addr)) {
		mmput(mm);
		return addr;
	}

	gpu->mm = mm;
	gpu->umem = (void __user *)addr;
	return 0;
}

/*
 * Return the DRM capset. As in virglrenderer, a device that cannot be
 * queried gives a capset of zeros, whose context type no driver accepts.
 */
static const struct vduse_guest_gpu_capset *
vduse_guest_gpu_capset(struct vduse_guest_gpu *gpu)
{
	static const struct vduse_guest_gpu_capset none;
	int err;

	if (gpu->capset_valid)
		return &gpu->capset;

	err = vduse_guest_gpu_amdgpu_capset(gpu, &gpu->capset);
	if (err) {
		pr_warn_ratelimited("%s: cannot query %s: %d\n",
				    vduse_guest_name(gpu->dev),
				    gpu->render_path, err);
		return &none;
	}
	gpu->capset_valid = true;
	return &gpu->capset;
}

static u32 vduse_guest_gpu_ctx_create(struct vduse_guest_gpu *gpu,
				      const struct virtio_gpu_ctx_create *cmd)
{
	u32 id = le32_to_cpu(cmd->hdr.ctx_id);
	struct vduse_guest_gpu_ctx *ctx;
	int err;

	if (le32_to_cpu(cmd->nlen) > sizeof(cmd->debug_name) ||
	    le32_to_cpu(cmd->context_init) != VIRTIO_GPU_CAPSET_DRM)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	if (!id || xa_load(&gpu->contexts, id))
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	if (atomic_read(&gpu->stats.contexts) == VDUSE_GUEST_GPU_MAX_CONTEXTS)
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	ctx->id = id;
	vduse_guest_gpu_amdgpu_ctx_init(gpu, ctx);

	err = xa_insert(&gpu->contexts, id, ctx, GFP_KERNEL);
	if (err) {
		vduse_guest_gpu_amdgpu_ctx_fini(gpu, ctx);
		kfree(ctx);
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	}
	atomic_inc(&gpu->stats.contexts);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

static void vduse_guest_gpu_ctx_free(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx)
{
	vduse_guest_gpu_amdgpu_ctx_fini(gpu, ctx);
	atomic_dec(&gpu->stats.contexts);
	kfree(ctx);
}

static u32 vduse_guest_gpu_ctx_destroy(struct vduse_guest_gpu *gpu,
				       const struct virtio_gpu_ctx_destroy *cmd)
{
	struct vduse_guest_gpu_ctx *ctx;

	ctx = xa_erase(&gpu->contexts, le32_to_cpu(cmd->hdr.ctx_id));
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	vduse_guest_gpu_ctx_free(gpu, ctx);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Map ranges into the region, which cover a range of it in order from
 * offset. VDUSE takes at most VDUSE_GUEST_GPU_MAP_BATCH ranges in a call, so
 * they go in batches. If a batch fails, the batches that were mapped are
 * unmapped again, and stuck tells whether that unmap failed, so that the
 * host may still map their memory.
 */
static int vduse_guest_gpu_map_ranges(struct vduse_guest_gpu *gpu, u64 offset,
				      const struct vduse_guest_shm_range *ranges,
				      unsigned long nr, bool *stuck)
{
	unsigned long done, n;
	u64 end = offset;
	int err = 0;

	for (done = 0; done < nr; done += n) {
		n = min(nr - done, VDUSE_GUEST_GPU_MAP_BATCH);
		err = vduse_guest_shm_map(gpu->dev,
					  VIRTIO_GPU_SHM_ID_HOST_VISIBLE,
					  ranges + done, n);
		if (err)
			break;
		end = ranges[done + n - 1].offset + ranges[done + n - 1].size;
	}

	*stuck = err && end > offset &&
		 vduse_guest_shm_unmap(gpu->dev,
				       VIRTIO_GPU_SHM_ID_HOST_VISIBLE, offset,
				       end - offset);
	return err;
}

/*
 * Map pages into the region at offset, one range for each run of contiguous
 * pages, split at 4 GiB, the largest range that VDUSE takes. The host maps
 * guest memory write-back, which matches the GTT BOs of a guest that maps no
 * GTT page write-combined.
 */
static int vduse_guest_gpu_map_pages(struct vduse_guest_gpu *gpu, u64 offset,
				     struct page **pages,
				     unsigned long nr_pages, bool *stuck)
{
	struct vduse_guest_shm_range *ranges;
	unsigned long i, nr = 0;
	int err;

	*stuck = false;
	ranges = kvcalloc(nr_pages, sizeof(*ranges), GFP_KERNEL);
	if (!ranges)
		return -ENOMEM;
	for (i = 0; i < nr_pages; i++) {
		phys_addr_t phys = page_to_phys(pages[i]);

		if (nr && ranges[nr - 1].phys + ranges[nr - 1].size == phys &&
		    ranges[nr - 1].size < SZ_4G) {
			ranges[nr - 1].size += PAGE_SIZE;
			continue;
		}
		ranges[nr].offset = offset + ((u64)i << PAGE_SHIFT);
		ranges[nr].size = PAGE_SIZE;
		ranges[nr].phys = phys;
		nr++;
	}

	err = vduse_guest_gpu_map_ranges(gpu, offset, ranges, nr, stuck);
	kvfree(ranges);
	return err;
}

/*
 * The ranges of BAR0 behind a BO in VRAM, split at 4 GiB. A first pass
 * counts them without ranges, and a second fills ranges with at most max of
 * them. Pos is the offset in the BO that the ranges reach.
 */
struct vduse_guest_gpu_vram_map {
	struct vduse_guest_gpu_res *res;
	struct vduse_guest_shm_range *ranges;
	unsigned long nr, max;
	u64 pos;
};

static int vduse_guest_gpu_vram_range(void *arg, u64 offset, u64 size)
{
	struct vduse_guest_gpu_vram_map *m = arg;
	struct vduse_guest_gpu_res *res = m->res;
	u64 n;

	for (size = min(size, res->size - m->pos); size; size -= n) {
		n = min_t(u64, size, SZ_4G);
		if (m->ranges) {
			if (m->nr == m->max)
				return -EINVAL;
			m->ranges[m->nr].offset = res->map_offset + m->pos;
			m->ranges[m->nr].size = n;
			m->ranges[m->nr].phys = offset;
			m->ranges[m->nr].file = res->gpu->bar_file;
		}
		m->nr++;
		m->pos += n;
		offset += n;
	}
	return 0;
}

/*
 * Map a reserved BO in CPU-visible VRAM at its placement in the region, as
 * ranges of BAR0. The host maps them write-combined, which x86 makes
 * uncached while vfio-pci maps BAR0 uncached.
 */
static int vduse_guest_gpu_map_vram(struct vduse_guest_gpu_res *res,
				    bool *stuck)
{
	struct vduse_guest_gpu_vram_map m = { .res = res };
	int err;

	*stuck = false;
	err = vduse_guest_gpu_amdgpu_vram(res, vduse_guest_gpu_vram_range, &m);
	if (err)
		return err;
	if (m.pos != res->size)
		return -EINVAL;

	m.ranges = kvcalloc(m.nr, sizeof(*m.ranges), GFP_KERNEL);
	if (!m.ranges)
		return -ENOMEM;
	m.max = m.nr;
	m.nr = 0;
	m.pos = 0;
	err = vduse_guest_gpu_amdgpu_vram(res, vduse_guest_gpu_vram_range, &m);
	if (!err && m.pos != res->size)
		err = -EINVAL;
	if (!err)
		err = vduse_guest_gpu_map_ranges(res->gpu, res->map_offset,
						 m.ranges, m.nr, stuck);
	kvfree(m.ranges);
	return err;
}

/*
 * Mark a reserved BO leaked and pin it, so that its pages stay with it while
 * the host may still map them.
 */
static void vduse_guest_gpu_bo_leak(struct vduse_guest_gpu_res *res)
{
	int err;

	res->leaked = true;
	err = vduse_guest_gpu_amdgpu_pin(res);
	if (err)
		pr_err("%s: cannot pin the leaked BO of resource %u: %d\n",
		       vduse_guest_name(res->gpu->dev), res->id, err);
}

/*
 * Map a reserved BO, in VRAM or in GTT as it was created, at its placement
 * in the region.
 */
static int vduse_guest_gpu_bo_map(struct vduse_guest_gpu_res *res)
{
	struct page **pages;
	bool stuck = false;
	int err;

	if (res->bo_domains == AMDGPU_GEM_DOMAIN_VRAM) {
		err = vduse_guest_gpu_map_vram(res, &stuck);
	} else {
		err = vduse_guest_gpu_amdgpu_pages(res, &pages);
		if (!err)
			err = vduse_guest_gpu_map_pages(res->gpu,
							res->map_offset, pages,
							res->size >> PAGE_SHIFT,
							&stuck);
	}
	if (stuck)
		vduse_guest_gpu_bo_leak(res);
	if (!err)
		res->present = true;
	return err;
}

/*
 * Unmap a reserved BO's pages. Once the unmap returns, the host maps none
 * of them. VDUSE fails the unmap of a range that the backend mapped only
 * through a bug, and the host may then still map pages that TTM reuses.
 */
static void vduse_guest_gpu_bo_unmap(struct vduse_guest_gpu_res *res)
{
	int err;

	if (!res->present)
		return;
	err = vduse_guest_shm_unmap(res->gpu->dev,
				    VIRTIO_GPU_SHM_ID_HOST_VISIBLE,
				    res->map_offset, res->size);
	res->present = false;
	if (err) {
		pr_err("%s: cannot unmap the BO of resource %u: %d\n",
		       vduse_guest_name(res->gpu->dev), res->id, err);
		res->leaked = true;
	}
}

/*
 * Called with the BO reserved before it moves, which frees its pages or
 * gives it others. A fault request maps it again.
 */
static void vduse_guest_gpu_bo_moved(void *priv)
{
	struct vduse_guest_gpu_res *res = priv;

	if (!res->present)
		return;
	vduse_guest_gpu_bo_unmap(res);
	atomic64_inc(&res->gpu->stats.moves);
}

/* Add or remove a BO's placement in the index that fault requests use. */
static int vduse_guest_gpu_index_add(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_res *res)
{
	unsigned long first = res->map_offset >> PAGE_SHIFT;
	int err;

	down_write(&gpu->fault_sem);
	err = mtree_insert_range(&gpu->fault_index, first,
				 first + (res->size >> PAGE_SHIFT) - 1, res,
				 GFP_KERNEL);
	up_write(&gpu->fault_sem);
	return err;
}

static void vduse_guest_gpu_index_del(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_res *res)
{
	down_write(&gpu->fault_sem);
	mtree_erase(&gpu->fault_index, res->map_offset >> PAGE_SHIFT);
	up_write(&gpu->fault_sem);
}

/*
 * Unmap a resource from the region. Once the unmap returns, the host maps
 * none of its pages. If the unmap fails, the host may still map them, so
 * the resource leaks them.
 */
static void vduse_guest_gpu_res_unmap(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_res *res)
{
	int err;

	if (!res->mapped)
		return;

	if (res->obj) {
		vduse_guest_gpu_index_del(gpu, res);
		dma_resv_lock(res->obj->resv, NULL);
		if (res->present) {
			vduse_guest_gpu_bo_unmap(res);
			if (res->leaked)
				vduse_guest_gpu_bo_leak(res);
		}
		vduse_guest_gpu_amdgpu_set_notify(res, NULL);
		res->mapped = false;
		dma_resv_unlock(res->obj->resv);
		return;
	}

	err = vduse_guest_shm_unmap(gpu->dev, VIRTIO_GPU_SHM_ID_HOST_VISIBLE,
				    res->map_offset, res->size);
	res->mapped = false;
	if (err) {
		pr_warn("%s: cannot unmap resource %u: %d\n",
			vduse_guest_name(gpu->dev), res->id, err);
		res->leaked = true;
	}
}

/*
 * A resource that is freed leaves every context, so that a later resource
 * with its id does not reach its BO through a context's handle.
 */
static void vduse_guest_gpu_res_free(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_res *res)
{
	struct vduse_guest_gpu_ctx *ctx;
	unsigned long id;
	unsigned long i;

	xa_for_each(&gpu->contexts, id, ctx) {
		if (ctx->shmem == res)
			ctx->shmem = NULL;
		vduse_guest_gpu_amdgpu_detach(ctx, res->id);
	}
	vduse_guest_gpu_res_unmap(gpu, res);
	if (!res->leaked) {
		if (res->obj) {
			drm_gem_object_put(res->obj);
		} else {
			if (res->vaddr)
				vunmap(res->vaddr);
			for (i = 0; i < res->nr_pages; i++)
				__free_page(res->pages[i]);
			gpu->shmem_size -= res->nr_pages << PAGE_SHIFT;
			kvfree(res->pages);
		}
	}
	atomic_dec(&gpu->stats.resources);
	kfree(res);
}

/*
 * Create blob 0, the context's shared memory buffer, as in virglrenderer,
 * from zeroed guest pages with virglrenderer's header.
 */
static int vduse_guest_gpu_create_shmem(struct vduse_guest_gpu *gpu,
					struct vduse_guest_gpu_ctx *ctx,
					struct vduse_guest_gpu_res *res)
{
	struct vduse_guest_gpu_shmem *shmem;
	unsigned long i, n = res->size >> PAGE_SHIFT;

	if (ctx->shmem || res->size < sizeof(*shmem) ||
	    res->size > VDUSE_GUEST_GPU_SHMEM_MAX)
		return -EINVAL;
	if (res->size > VDUSE_GUEST_GPU_SHMEM_TOTAL - gpu->shmem_size)
		return -ENOMEM;

	res->pages = kvcalloc(n, sizeof(*res->pages), GFP_KERNEL);
	if (!res->pages)
		return -ENOMEM;
	for (i = 0; i < n; i++) {
		res->pages[i] = alloc_page(GFP_KERNEL | __GFP_ZERO);
		if (!res->pages[i])
			return -ENOMEM;
		res->nr_pages++;
		gpu->shmem_size += PAGE_SIZE;
	}
	res->vaddr = vmap(res->pages, res->nr_pages, VM_MAP, PAGE_KERNEL);
	if (!res->vaddr)
		return -ENOMEM;

	shmem = res->vaddr;
	shmem->rsp_mem_offset = sizeof(*shmem);
	return 0;
}

/*
 * Create a HOST3D blob: blob 0, the context's shared memory buffer, or the
 * BO of the context's GEM_NEW request that has the blob's id.
 */
static u32 vduse_guest_gpu_create_blob(struct vduse_guest_gpu *gpu,
				       const struct virtio_gpu_resource_create_blob *cmd)
{
	u32 ctx_id = le32_to_cpu(cmd->hdr.ctx_id);
	u32 id = le32_to_cpu(cmd->resource_id);
	u64 blob_id = le64_to_cpu(cmd->blob_id);
	u64 size = le64_to_cpu(cmd->size);
	struct vduse_guest_gpu_res *res;
	struct vduse_guest_gpu_ctx *ctx;
	int err;

	ctx = xa_load(&gpu->contexts, ctx_id);
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	if (!id || xa_load(&gpu->resources, id))
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	if (le32_to_cpu(cmd->blob_mem) != VIRTIO_GPU_BLOB_MEM_HOST3D ||
	    le32_to_cpu(cmd->nr_entries) || blob_id > U32_MAX || !size ||
	    !PAGE_ALIGNED(size))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	res = kzalloc_obj(*res);
	if (!res)
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	res->gpu = gpu;
	res->id = id;
	res->blob_flags = le32_to_cpu(cmd->blob_flags);
	res->size = size;
	atomic_inc(&gpu->stats.resources);

	if (blob_id)
		err = vduse_guest_gpu_amdgpu_create_bo(gpu, ctx, res, blob_id);
	else
		err = vduse_guest_gpu_create_shmem(gpu, ctx, res);
	if (!err)
		err = xa_insert(&gpu->resources, id, res, GFP_KERNEL);
	if (err) {
		vduse_guest_gpu_res_free(gpu, res);
		return err == -ENOMEM ? VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY :
					VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	}

	if (!blob_id) {
		ctx->shmem = res;
		vduse_guest_gpu_amdgpu_update_heaps(gpu, ctx);
	}
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Place a mappable blob at the offset that the host's driver chose and map
 * it. A BO is added to the index of fault requests before it is reserved,
 * since a fault request holds fault_sem while it reserves the BO. Its moves
 * unmap it again until it is unmapped. BAR0 is registered before the first
 * BO in VRAM is mapped, and its ranges are uncached on the host.
 */
static u32 vduse_guest_gpu_map_blob(struct vduse_guest_gpu *gpu,
				    const struct virtio_gpu_resource_map_blob *cmd,
				    struct virtio_gpu_resp_map_info *info)
{
	u64 offset = le64_to_cpu(cmd->offset);
	struct vduse_guest_gpu_res *res;
	bool stuck = false, vram = false;
	int err;

	res = xa_load(&gpu->resources, le32_to_cpu(cmd->resource_id));
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	if (!(res->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) ||
	    res->mapped || res->leaked || !PAGE_ALIGNED(offset))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	res->map_offset = offset;

	if (res->obj) {
		vram = gpu->vram && res->bo_domains == AMDGPU_GEM_DOMAIN_VRAM &&
		       !(res->bo_flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS);
		err = vram && !gpu->bar_file ?
			vduse_guest_gpu_amdgpu_reg_bar(res, &gpu->bar_file) : 0;
		if (!err)
			err = vduse_guest_gpu_index_add(gpu, res);
		if (err)
			goto fail;
		dma_resv_lock(res->obj->resv, NULL);
		err = vduse_guest_gpu_bo_map(res);
		if (!err) {
			res->mapped = true;
			vduse_guest_gpu_amdgpu_set_notify(res,
							  vduse_guest_gpu_bo_moved);
		}
		dma_resv_unlock(res->obj->resv);
		if (err)
			vduse_guest_gpu_index_del(gpu, res);
	} else {
		err = vduse_guest_gpu_map_pages(gpu, offset, res->pages,
						res->nr_pages, &stuck);
		if (stuck)
			res->leaked = true;
		if (!err)
			res->mapped = true;
	}
	if (err)
		goto fail;

	info->map_info = cpu_to_le32(vram ? VIRTIO_GPU_MAP_CACHE_UNCACHED :
					    VIRTIO_GPU_MAP_CACHE_CACHED);
	return VIRTIO_GPU_RESP_OK_MAP_INFO;

fail:
	return err == -ENOMEM ? VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY :
				VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
}

static u32 vduse_guest_gpu_unmap_blob(struct vduse_guest_gpu *gpu,
				      const struct virtio_gpu_resource_unmap_blob *cmd)
{
	struct vduse_guest_gpu_res *res;

	res = xa_load(&gpu->resources, le32_to_cpu(cmd->resource_id));
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	vduse_guest_gpu_res_unmap(gpu, res);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

static u32 vduse_guest_gpu_unref(struct vduse_guest_gpu *gpu,
				 const struct virtio_gpu_resource_unref *cmd)
{
	struct vduse_guest_gpu_res *res;

	res = xa_erase(&gpu->resources, le32_to_cpu(cmd->resource_id));
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	vduse_guest_gpu_res_free(gpu, res);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Attaching a resource gives the context a handle to its BO, unless it has
 * one, and detaching deletes it. Detaching a resource that is not attached
 * does nothing.
 */
static u32 vduse_guest_gpu_ctx_resource(struct vduse_guest_gpu *gpu,
					const struct virtio_gpu_ctx_resource *cmd)
{
	u32 res_id = le32_to_cpu(cmd->resource_id);
	struct vduse_guest_gpu_res *res;
	struct vduse_guest_gpu_ctx *ctx;

	ctx = xa_load(&gpu->contexts, le32_to_cpu(cmd->hdr.ctx_id));
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	if (le32_to_cpu(cmd->hdr.type) == VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE) {
		vduse_guest_gpu_amdgpu_detach(ctx, res_id);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}

	res = xa_load(&gpu->resources, res_id);
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	if (vduse_guest_gpu_amdgpu_attach(ctx, res))
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Run the ccmds of a SUBMIT_3D, whose stream follows the command in the
 * chain. A fenced command without a ring belongs to the device's timeline.
 */
static u32 vduse_guest_gpu_submit_3d(struct vduse_guest_gpu *gpu,
				     const struct virtio_gpu_cmd_submit *cmd,
				     struct vringh_kiov *riov, size_t in_len)
{
	u32 flags = le32_to_cpu(cmd->hdr.flags);
	u32 size = le32_to_cpu(cmd->size);
	struct vduse_guest_gpu_ctx *ctx;
	struct iov_iter it;
	void *buf;
	int err;

	ctx = xa_load(&gpu->contexts, le32_to_cpu(cmd->hdr.ctx_id));
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	if (size > VDUSE_GUEST_GPU_SUBMIT_MAX || size > in_len - sizeof(*cmd))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	buf = kvmalloc(size, GFP_KERNEL);
	if (!buf)
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, in_len);
	iov_iter_advance(&it, sizeof(*cmd));
	if (copy_from_iter(buf, size, &it) != size) {
		kvfree(buf);
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	}

	err = vduse_guest_gpu_amdgpu_submit(gpu, ctx, buf, size,
					    (flags & VIRTIO_GPU_FLAG_FENCE) &&
					    !(flags & VIRTIO_GPU_FLAG_INFO_RING_IDX));
	kvfree(buf);
	return err ? VIRTIO_GPU_RESP_ERR_UNSPEC : VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Run a command of in_len bytes, whose start is in req and whole chain in
 * riov, and return the type of its response. A response with data points
 * out at it.
 */
static u32 vduse_guest_gpu_dispatch(struct vduse_guest_gpu *gpu,
				    const union vduse_guest_gpu_req *req,
				    struct vringh_kiov *riov, size_t in_len,
				    const void **out, size_t *out_len)
{
	static const struct virtio_gpu_resp_capset_info capset_info = {
		.capset_id = cpu_to_le32(VIRTIO_GPU_CAPSET_DRM),
		.capset_max_size =
			cpu_to_le32(sizeof(struct vduse_guest_gpu_capset)),
	};

	switch (le32_to_cpu(req->hdr.type)) {
	case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
		if (in_len < sizeof(req->get_capset_info))
			break;
		if (le32_to_cpu(req->get_capset_info.capset_index))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		*out = (void *)&capset_info + sizeof(capset_info.hdr);
		*out_len = sizeof(capset_info) - sizeof(capset_info.hdr);
		return VIRTIO_GPU_RESP_OK_CAPSET_INFO;
	case VIRTIO_GPU_CMD_GET_CAPSET:
		if (in_len < sizeof(req->get_capset))
			break;
		if (le32_to_cpu(req->get_capset.capset_id) !=
			    VIRTIO_GPU_CAPSET_DRM ||
		    le32_to_cpu(req->get_capset.capset_version))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		*out = vduse_guest_gpu_capset(gpu);
		*out_len = sizeof(struct vduse_guest_gpu_capset);
		return VIRTIO_GPU_RESP_OK_CAPSET;
	case VIRTIO_GPU_CMD_CTX_CREATE:
		if (in_len < sizeof(req->ctx_create))
			break;
		return vduse_guest_gpu_ctx_create(gpu, &req->ctx_create);
	case VIRTIO_GPU_CMD_CTX_DESTROY:
		return vduse_guest_gpu_ctx_destroy(gpu, &req->ctx_destroy);
	case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
	case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
		if (in_len < sizeof(req->ctx_resource))
			break;
		return vduse_guest_gpu_ctx_resource(gpu, &req->ctx_resource);
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
		if (in_len < sizeof(req->create_blob))
			break;
		return vduse_guest_gpu_create_blob(gpu, &req->create_blob);
	case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB:
		if (in_len < sizeof(req->map_blob))
			break;
		memset(&gpu->map_info, 0, sizeof(gpu->map_info));
		*out = (void *)&gpu->map_info + sizeof(gpu->map_info.hdr);
		*out_len = sizeof(gpu->map_info) - sizeof(gpu->map_info.hdr);
		return vduse_guest_gpu_map_blob(gpu, &req->map_blob,
						&gpu->map_info);
	case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB:
		if (in_len < sizeof(req->unmap_blob))
			break;
		return vduse_guest_gpu_unmap_blob(gpu, &req->unmap_blob);
	case VIRTIO_GPU_CMD_RESOURCE_UNREF:
		if (in_len < sizeof(req->unref))
			break;
		return vduse_guest_gpu_unref(gpu, &req->unref);
	case VIRTIO_GPU_CMD_SUBMIT_3D:
		if (in_len < sizeof(req->submit))
			break;
		return vduse_guest_gpu_submit_3d(gpu, &req->submit, riov, in_len);
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		*out = (void *)&gpu->display_info + sizeof(gpu->display_info.hdr);
		*out_len = sizeof(gpu->display_info) -
			   sizeof(gpu->display_info.hdr);
		return vduse_guest_gpu_display_info(gpu, &gpu->display_info);
	case VIRTIO_GPU_CMD_GET_EDID:
		if (in_len < sizeof(req->get_edid))
			break;
		*out = (void *)&gpu->edid + sizeof(gpu->edid.hdr);
		*out_len = sizeof(gpu->edid) - sizeof(gpu->edid.hdr);
		return vduse_guest_gpu_get_edid(gpu, &req->get_edid, &gpu->edid);
	case VIRTIO_GPU_CMD_SET_SCANOUT:
		if (in_len < sizeof(req->set_scanout))
			break;
		return vduse_guest_gpu_set_scanout(gpu, &req->set_scanout);
	case VIRTIO_GPU_CMD_SET_SCANOUT_BLOB:
		if (in_len < sizeof(req->set_scanout_blob))
			break;
		return vduse_guest_gpu_set_scanout_blob(gpu,
							&req->set_scanout_blob);
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
		/* A fenced flush waits for the flip of the resource. */
		if (in_len < sizeof(req->flush))
			break;
		return gpu->display ? VIRTIO_GPU_RESP_OK_NODATA :
				      VIRTIO_GPU_RESP_ERR_UNSPEC;
	}

	return VIRTIO_GPU_RESP_ERR_UNSPEC;
}

/*
 * Handle a command chain and return the length written into its response.
 * The response to a fenced command carries its fence, also when it is an
 * error, since the host's driver completes fences from the response. A
 * fenced command on a ring of a context completes once the last command
 * submission on that ring's timeline has finished, which *fence gives. A
 * fenced RESOURCE_FLUSH without a ring completes at the latest flip of a
 * scanout that shows the resource, whose fence *fence gives.
 */
static u32 vduse_guest_gpu_handle(struct vduse_guest_gpu *gpu,
				  struct vringh_kiov *riov,
				  struct vringh_kiov *wiov,
				  struct dma_fence **fence)
{
	size_t in_len = vringh_kiov_length(riov);
	size_t room = vringh_kiov_length(wiov);
	struct virtio_gpu_ctrl_hdr resp = {};
	union vduse_guest_gpu_req req = {};
	struct vduse_guest_gpu_ctx *ctx;
	const void *out = NULL;
	size_t n, out_len = 0;
	struct iov_iter it;
	u32 type, flags;

	atomic64_inc(&gpu->stats.commands);

	n = min(in_len, sizeof(req));
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, in_len);
	if (n < sizeof(req.hdr) || copy_from_iter(&req, n, &it) != n) {
		type = VIRTIO_GPU_RESP_ERR_UNSPEC;
	} else {
		type = vduse_guest_gpu_dispatch(gpu, &req, riov, in_len, &out,
						&out_len);
		flags = le32_to_cpu(req.hdr.flags);
		if (flags & VIRTIO_GPU_FLAG_FENCE) {
			resp.flags = cpu_to_le32(VIRTIO_GPU_FLAG_FENCE);
			resp.fence_id = req.hdr.fence_id;
			resp.ctx_id = req.hdr.ctx_id;
		}
		if ((flags & VIRTIO_GPU_FLAG_FENCE) &&
		    !(flags & VIRTIO_GPU_FLAG_INFO_RING_IDX) &&
		    type == VIRTIO_GPU_RESP_OK_NODATA &&
		    le32_to_cpu(req.hdr.type) == VIRTIO_GPU_CMD_RESOURCE_FLUSH)
			*fence = vduse_guest_gpu_flush_fence(gpu,
				le32_to_cpu(req.flush.resource_id));
		if ((flags & VIRTIO_GPU_FLAG_FENCE) &&
		    (flags & VIRTIO_GPU_FLAG_INFO_RING_IDX)) {
			resp.flags |= cpu_to_le32(VIRTIO_GPU_FLAG_INFO_RING_IDX);
			resp.ring_idx = req.hdr.ring_idx;
			ctx = xa_load(&gpu->contexts,
				      le32_to_cpu(req.hdr.ctx_id));
			if (ctx)
				*fence = vduse_guest_gpu_amdgpu_fence(ctx,
								      req.hdr.ring_idx);
		}
	}

	if (room < sizeof(resp))
		return 0;
	if (sizeof(resp) + out_len > room) {
		type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		out_len = 0;
	}
	if (type >= VIRTIO_GPU_RESP_ERR_UNSPEC) {
		atomic64_inc(&gpu->stats.invalid_commands);
		out_len = 0;
	}
	resp.type = cpu_to_le32(type);

	iov_iter_kvec(&it, ITER_DEST, wiov->iov, wiov->used, room);
	/* The room was checked, so a short copy means the buffer faulted. */
	if (copy_to_iter(&resp, sizeof(resp), &it) != sizeof(resp) ||
	    copy_to_iter(out, out_len, &it) != out_len)
		return 0;
	return sizeof(resp) + out_len;
}

bool vduse_guest_gpu_running(struct vduse_guest_gpu *gpu)
{
	bool running;

	spin_lock_irq(&gpu->lock);
	running = gpu->running;
	spin_unlock_irq(&gpu->lock);
	return running;
}

/* A signaled fence's command is completed by the fence work, in order. */
static void vduse_guest_gpu_fence_signaled(struct dma_fence *fence,
					   struct dma_fence_cb *cb)
{
	struct vduse_guest_gpu_fence *f =
		container_of(cb, struct vduse_guest_gpu_fence, cb);
	struct vduse_guest_gpu *gpu = f->gpu;
	unsigned long flags;

	spin_lock_irqsave(&gpu->fence_lock, flags);
	list_move_tail(&f->list, &gpu->signaled);
	spin_unlock_irqrestore(&gpu->fence_lock, flags);
	kthread_queue_work(gpu->worker, &gpu->fence_work);
}

static void vduse_guest_gpu_fence_free(struct vduse_guest_gpu_fence *f)
{
	dma_fence_put(f->fence);
	kfree(f);
}

static void vduse_guest_gpu_fence_work(struct kthread_work *work)
{
	struct vduse_guest_gpu *gpu =
		container_of(work, struct vduse_guest_gpu, fence_work);
	struct vduse_guest_gpu_fence *f;

	while (vduse_guest_gpu_running(gpu)) {
		spin_lock_irq(&gpu->fence_lock);
		f = list_first_entry_or_null(&gpu->signaled,
					     struct vduse_guest_gpu_fence, list);
		if (f)
			list_del(&f->list);
		spin_unlock_irq(&gpu->fence_lock);
		if (!f)
			break;
		vduse_guest_complete(gpu->dev, VDUSE_GUEST_GPU_CTRLQ, f->head,
				     f->len);
		vduse_guest_gpu_fence_free(f);
	}
}

/*
 * Complete a command once its fence signals. A command whose fence has
 * signaled also goes through the fence work, behind the commands whose
 * fences signaled before, so that a timeline's commands complete in order.
 * If the allocation for the wait fails, the worker waits for the fence
 * and completes the command at once.
 */
static void vduse_guest_gpu_complete(struct vduse_guest_gpu *gpu, u16 head,
				     u32 len, struct dma_fence *fence)
{
	struct vduse_guest_gpu_fence *f;

	if (!fence)
		goto now;
	f = kzalloc_obj(*f);
	if (!f) {
		dma_fence_wait(fence, false);
		goto now;
	}
	f->gpu = gpu;
	f->fence = fence;
	f->head = head;
	f->len = len;

	spin_lock_irq(&gpu->fence_lock);
	list_add_tail(&f->list, &gpu->waiting);
	spin_unlock_irq(&gpu->fence_lock);
	if (dma_fence_add_callback(fence, &f->cb,
				   vduse_guest_gpu_fence_signaled))
		vduse_guest_gpu_fence_signaled(fence, &f->cb);
	return;

now:
	dma_fence_put(fence);
	vduse_guest_complete(gpu->dev, VDUSE_GUEST_GPU_CTRLQ, head, len);
}

/* A command of the cursor queue has no response. */
static void vduse_guest_gpu_cursor_cmd(struct vduse_guest_gpu *gpu,
				       struct vringh_kiov *riov)
{
	size_t len = vringh_kiov_length(riov);
	struct virtio_gpu_update_cursor cmd;
	struct iov_iter it;

	if (!gpu->display)
		return;
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, len);
	if (len < sizeof(cmd) || copy_from_iter(&cmd, sizeof(cmd), &it) !=
				 sizeof(cmd)) {
		atomic64_inc(&gpu->stats.cursor_errors);
		return;
	}
	vduse_guest_gpu_cursor(gpu, &cmd);
}

/*
 * Serve one queue until it is empty. Each command is answered with the
 * length that handle returns, and the cursor queue's commands, which have no
 * response, are completed without one.
 */
static void vduse_guest_gpu_serve(struct vduse_guest_gpu *gpu,
				  unsigned int index)
{
	struct kvec rkvec[VDUSE_GUEST_GPU_INLINE_IOVS];
	struct kvec wkvec[VDUSE_GUEST_GPU_INLINE_IOVS];
	struct vringh_kiov riov, wiov;
	struct dma_fence *fence;
	u16 head;
	u32 len;
	int r;

	vringh_kiov_init(&riov, rkvec, ARRAY_SIZE(rkvec));
	vringh_kiov_init(&wiov, wkvec, ARRAY_SIZE(wkvec));

	while (vduse_guest_gpu_running(gpu)) {
		r = vduse_guest_getdesc(gpu->dev, index, &riov, &wiov, &head);
		if (r < 0)
			break;
		if (!r) {
			if (vduse_guest_notify_enable(gpu->dev, index))
				break;
			vduse_guest_notify_disable(gpu->dev, index);
			continue;
		}

		if (index == VDUSE_GUEST_GPU_CTRLQ) {
			fence = NULL;
			len = vduse_guest_gpu_handle(gpu, &riov, &wiov, &fence);
			vduse_guest_gpu_complete(gpu, head, len, fence);
		} else {
			vduse_guest_gpu_cursor_cmd(gpu, &riov);
			vduse_guest_complete(gpu->dev, index, head, 0);
		}
		cond_resched();
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

/* Commands run their ioctls in the worker's mm, which it adopts meanwhile. */
static void vduse_guest_gpu_ctrl_work(struct kthread_work *work)
{
	struct vduse_guest_gpu *gpu =
		container_of(work, struct vduse_guest_gpu, ctrl_work);

	if (vduse_guest_gpu_init_mm(gpu))
		pr_warn_ratelimited("%s: cannot create the worker's mm\n",
				    vduse_guest_name(gpu->dev));
	if (gpu->mm)
		kthread_use_mm(gpu->mm);
	vduse_guest_gpu_serve(gpu, VDUSE_GUEST_GPU_CTRLQ);
	if (gpu->mm)
		kthread_unuse_mm(gpu->mm);
}

static void vduse_guest_gpu_cursor_work(struct kthread_work *work)
{
	struct vduse_guest_gpu *gpu =
		container_of(work, struct vduse_guest_gpu, cursor_work);

	vduse_guest_gpu_serve(gpu, VDUSE_GUEST_GPU_CURSORQ);
}

static void vduse_guest_gpu_kick(void *priv, unsigned int index)
{
	struct vduse_guest_gpu *gpu = priv;

	if (index == VDUSE_GUEST_GPU_CTRLQ)
		kthread_queue_work(gpu->worker, &gpu->ctrl_work);
	else
		kthread_queue_work(gpu->worker, &gpu->cursor_work);
}

static int vduse_guest_gpu_start(void *priv)
{
	struct vduse_guest_gpu *gpu = priv;

	spin_lock_irq(&gpu->lock);
	gpu->running = true;
	spin_unlock_irq(&gpu->lock);
	return 0;
}

/*
 * Drop the commands that wait for fences, which the reset discards. A
 * callback that could not be removed has already moved its command to
 * signaled.
 */
static void vduse_guest_gpu_drop_fences(struct vduse_guest_gpu *gpu)
{
	struct vduse_guest_gpu_fence *f, *tmp;
	LIST_HEAD(dropped);

	for (;;) {
		spin_lock_irq(&gpu->fence_lock);
		f = list_first_entry_or_null(&gpu->waiting,
					     struct vduse_guest_gpu_fence, list);
		if (f)
			list_del_init(&f->list);
		spin_unlock_irq(&gpu->fence_lock);
		if (!f)
			break;
		if (dma_fence_remove_callback(f->fence, &f->cb))
			vduse_guest_gpu_fence_free(f);
	}

	kthread_flush_worker(gpu->worker);
	spin_lock_irq(&gpu->fence_lock);
	list_splice_init(&gpu->signaled, &dropped);
	spin_unlock_irq(&gpu->fence_lock);
	list_for_each_entry_safe(f, tmp, &dropped, list)
		vduse_guest_gpu_fence_free(f);
}

/* A fault request that waits for the fault work. */
struct vduse_guest_gpu_fault {
	struct list_head list;
	struct vduse_guest_msg msg;
};

/*
 * Map the BO placed at a fault request's offset where it now is, unless it
 * is no longer placed there or the device has stopped, and return whether it
 * is mapped. The host drops every range of BAR0 while the PCI device's
 * memory is disabled, so a BO in VRAM that is mapped is mapped again, which
 * fails with -EEXIST before it maps anything when the host has its ranges.
 */
static bool vduse_guest_gpu_fault_map(struct vduse_guest_gpu *gpu,
				      const struct vduse_guest_msg *msg)
{
	struct vduse_guest_gpu_res *res;
	bool ok = false;
	int err;

	if (msg->index != VIRTIO_GPU_SHM_ID_HOST_VISIBLE ||
	    !vduse_guest_gpu_running(gpu))
		return false;

	down_read(&gpu->fault_sem);
	res = mtree_load(&gpu->fault_index, msg->start >> PAGE_SHIFT);
	if (res) {
		dma_resv_lock(res->obj->resv, NULL);
		if (res->mapped && !res->leaked && res->present &&
		    res->bo_domains == AMDGPU_GEM_DOMAIN_VRAM) {
			err = vduse_guest_gpu_bo_map(res);
			ok = !err || err == -EEXIST;
		} else if (res->mapped && !res->leaked) {
			ok = res->present || !vduse_guest_gpu_bo_map(res);
		}
		dma_resv_unlock(res->obj->resv);
	}
	up_read(&gpu->fault_sem);
	return ok;
}

static void vduse_guest_gpu_fault_work(struct kthread_work *work)
{
	struct vduse_guest_gpu *gpu =
		container_of(work, struct vduse_guest_gpu, fault_work);
	struct vduse_guest_gpu_fault *f;
	bool ok;

	for (;;) {
		spin_lock(&gpu->fault_lock);
		f = list_first_entry_or_null(&gpu->faults,
					     struct vduse_guest_gpu_fault, list);
		if (f)
			list_del(&f->list);
		spin_unlock(&gpu->fault_lock);
		if (!f)
			break;

		ok = vduse_guest_gpu_fault_map(gpu, &f->msg);
		atomic64_inc(ok ? &gpu->stats.faults :
				  &gpu->stats.faults_refused);
		vduse_guest_shm_fault_done(gpu->dev, &f->msg, ok);
		kfree(f);
		cond_resched();
	}
}

/*
 * Take a fault request on the host-visible region, which the fault work
 * answers.
 */
static void vduse_guest_gpu_shm_fault(void *priv,
				      const struct vduse_guest_msg *msg)
{
	struct vduse_guest_gpu *gpu = priv;
	struct vduse_guest_gpu_fault *f;

	f = kmalloc_obj(*f);
	if (!f) {
		atomic64_inc(&gpu->stats.faults_refused);
		vduse_guest_shm_fault_done(gpu->dev, msg, false);
		return;
	}
	f->msg = *msg;
	spin_lock(&gpu->fault_lock);
	list_add_tail(&f->list, &gpu->faults);
	spin_unlock(&gpu->fault_lock);
	kthread_queue_work(gpu->fault_worker, &gpu->fault_work);
}

/*
 * A reset drops the commands that wait for fences, refuses the fault
 * requests that wait, unmaps and frees every resource and destroys every
 * context.
 */
static void vduse_guest_gpu_stop(void *priv)
{
	struct vduse_guest_gpu *gpu = priv;
	struct vduse_guest_gpu_res *res;
	struct vduse_guest_gpu_ctx *ctx;
	unsigned long id;

	spin_lock_irq(&gpu->lock);
	gpu->running = false;
	spin_unlock_irq(&gpu->lock);
	kthread_flush_worker(gpu->worker);
	kthread_flush_worker(gpu->fault_worker);
	vduse_guest_gpu_drop_fences(gpu);
	vduse_guest_gpu_display_reset(gpu);

	xa_for_each(&gpu->resources, id, res) {
		xa_erase(&gpu->resources, id);
		vduse_guest_gpu_res_free(gpu, res);
	}
	xa_for_each(&gpu->contexts, id, ctx) {
		xa_erase(&gpu->contexts, id);
		vduse_guest_gpu_ctx_free(gpu, ctx);
	}
}

static void *__init vduse_guest_gpu_setup(struct vduse_guest_dev *dev,
					  char *opts,
					  struct vduse_guest_config *cfg)
{
	const char *name = vduse_guest_name(dev);
	const char *render = VDUSE_GUEST_GPU_RENDER;
	u64 region = VDUSE_GUEST_GPU_REGION_SIZE;
	struct vduse_guest_gpu *gpu;
	bool vram = false, display = false;
	char *opt, *end;
	int err;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "render=")) {
			render = opt + strlen("render=");
		} else if (!strcmp(opt, "vram")) {
			vram = true;
		} else if (!strcmp(opt, "display")) {
			display = true;
		} else if (strstarts(opt, "region=")) {
			region = memparse(opt + strlen("region="), &end);
			if (*end || !region || !PAGE_ALIGNED(region))
				return ERR_PTR(-EINVAL);
		} else if (*opt) {
			return ERR_PTR(-EINVAL);
		}
	}

	gpu = kzalloc_obj(*gpu);
	if (!gpu)
		return ERR_PTR(-ENOMEM);
	gpu->dev = dev;
	/*
	 * The display places mappable BOs in VRAM, since amdgpu scans out
	 * from VRAM on the iGPU.
	 */
	gpu->vram = vram || display;
	spin_lock_init(&gpu->lock);
	spin_lock_init(&gpu->fence_lock);
	INIT_LIST_HEAD(&gpu->waiting);
	INIT_LIST_HEAD(&gpu->signaled);
	xa_init(&gpu->contexts);
	xa_init(&gpu->resources);
	spin_lock_init(&gpu->fault_lock);
	INIT_LIST_HEAD(&gpu->faults);
	init_rwsem(&gpu->fault_sem);
	mt_init(&gpu->fault_index);
	kthread_init_work(&gpu->fault_work, vduse_guest_gpu_fault_work);
	kthread_init_work(&gpu->ctrl_work, vduse_guest_gpu_ctrl_work);
	kthread_init_work(&gpu->cursor_work, vduse_guest_gpu_cursor_work);
	kthread_init_work(&gpu->fence_work, vduse_guest_gpu_fence_work);

	gpu->render_path = kstrdup(render, GFP_KERNEL);
	if (!gpu->render_path) {
		err = -ENOMEM;
		goto free;
	}

	gpu->worker = kthread_run_worker(0, "vduse-gpu-%s", name);
	if (IS_ERR(gpu->worker)) {
		err = PTR_ERR(gpu->worker);
		goto free;
	}
	gpu->fault_worker = kthread_run_worker(0, "vduse-gpufault-%s", name);
	if (IS_ERR(gpu->fault_worker)) {
		err = PTR_ERR(gpu->fault_worker);
		kthread_destroy_worker(gpu->worker);
		goto free;
	}

	/*
	 * Without the display option, the device has no scanouts, which the
	 * host's driver accepts as a device without KMS.
	 */
	gpu->config.num_capsets = cpu_to_le32(1);
	if (display) {
		err = vduse_guest_gpu_display_init(gpu);
		if (err)
			pr_warn("%s: serves no scanouts: %d\n", name, err);
	}

	/*
	 * The host's driver accepts execbuffers, HOST3D blobs and context
	 * initialization only from a device that offers VIRGL, although
	 * native context does not run virgl.
	 */
	cfg->device_id = VIRTIO_ID_GPU;
	cfg->features = BIT_ULL(VIRTIO_GPU_F_VIRGL) |
			BIT_ULL(VIRTIO_GPU_F_RESOURCE_BLOB) |
			BIT_ULL(VIRTIO_GPU_F_CONTEXT_INIT);
	/*
	 * The host takes the modes of a scanout from its EDID, and paces its
	 * flips by the fences of its flushes.
	 */
	if (gpu->display)
		cfg->features |= BIT_ULL(VIRTIO_GPU_F_EDID) |
				 BIT_ULL(VIRTIO_GPU_F_FLIP_FENCE);
	cfg->num_queues = ARRAY_SIZE(vduse_guest_gpu_queue_sizes);
	cfg->queue_sizes = vduse_guest_gpu_queue_sizes;
	/* A new guest has none of the contexts of the one before it. */
	cfg->allow_reconnect = false;
	cfg->config = &gpu->config;
	cfg->config_size = sizeof(gpu->config);
	/*
	 * Blobs are mapped into the host-visible region, whose size bounds
	 * the total size of the mapped blobs and costs nothing by itself.
	 */
	gpu->region.id = VIRTIO_GPU_SHM_ID_HOST_VISIBLE;
	gpu->region.size = region;
	cfg->shm = &gpu->region;
	cfg->num_shm = 1;

	pr_info("%s: serving %s\n", name, gpu->render_path);
	return gpu;

free:
	kfree(gpu->render_path);
	kfree(gpu);
	return ERR_PTR(err);
}

static void vduse_guest_gpu_release(void *priv)
{
	struct vduse_guest_gpu *gpu = priv;

	vduse_guest_gpu_display_fini(gpu);
	kthread_destroy_worker(gpu->fault_worker);
	kthread_destroy_worker(gpu->worker);
	if (gpu->mm)
		mmput(gpu->mm);
	mtree_destroy(&gpu->fault_index);
	kfree(gpu->render_path);
	kfree(gpu);
}

static void vduse_guest_gpu_show(void *priv, struct seq_file *m)
{
	struct vduse_guest_gpu *gpu = priv;
	struct vduse_guest_gpu_stats *s = &gpu->stats;

	seq_printf(m, "  %-18s %lld\n", "commands", atomic64_read(&s->commands));
	seq_printf(m, "  %-18s %lld\n", "invalid_commands",
		   atomic64_read(&s->invalid_commands));
	seq_printf(m, "  %-18s %d\n", "contexts", atomic_read(&s->contexts));
	seq_printf(m, "  %-18s %d\n", "resources", atomic_read(&s->resources));
	seq_printf(m, "  %-18s %lld\n", "moves", atomic64_read(&s->moves));
	seq_printf(m, "  %-18s %lld\n", "faults", atomic64_read(&s->faults));
	seq_printf(m, "  %-18s %lld\n", "faults_refused",
		   atomic64_read(&s->faults_refused));
	seq_printf(m, "  %-18s %lld\n", "flips", atomic64_read(&s->flips));
	seq_printf(m, "  %-18s %lld\n", "cursor_updates",
		   atomic64_read(&s->cursor_updates));
	seq_printf(m, "  %-18s %lld\n", "cursor_moves",
		   atomic64_read(&s->cursor_moves));
	seq_printf(m, "  %-18s %lld\n", "cursor_errors",
		   atomic64_read(&s->cursor_errors));
	seq_printf(m, "  %-18s %lld\n", "flips_timed",
		   atomic64_read(&s->flips_timed));
	seq_printf(m, "  %-18s %lld\n", "flip_commit_us",
		   atomic64_read(&s->flip_commit_us));
	seq_printf(m, "  %-18s %lld\n", "flip_wait_us",
		   atomic64_read(&s->flip_wait_us));
}

/* The host's driver acknowledges display events by writing events_clear. */
static void vduse_guest_gpu_config_write(void *priv, u32 offset,
					 const void *buf, u32 len)
{
	__le32 events;

	if (offset != offsetof(struct virtio_gpu_config, events_clear) ||
	    len != sizeof(events))
		return;
	memcpy(&events, buf, sizeof(events));
	vduse_guest_gpu_display_clear(priv, le32_to_cpu(events));
}

const struct vduse_guest_backend_ops vduse_guest_gpu_ops = {
	.type = "gpu",
	.setup = vduse_guest_gpu_setup,
	.release = vduse_guest_gpu_release,
	.start = vduse_guest_gpu_start,
	.stop = vduse_guest_gpu_stop,
	.kick = vduse_guest_gpu_kick,
	.show = vduse_guest_gpu_show,
	.shm_fault = vduse_guest_gpu_shm_fault,
	.config_write = vduse_guest_gpu_config_write,
};
