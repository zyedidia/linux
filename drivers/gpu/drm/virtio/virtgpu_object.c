/*
 * Copyright (C) 2015 Red Hat, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <linux/dma-mapping.h>
#include <linux/moduleparam.h>

#include <drm/drm_print.h>

#include "virtgpu_drv.h"

static int virtio_gpu_virglrenderer_workaround = 1;
module_param_named(virglhack, virtio_gpu_virglrenderer_workaround, int, 0400);

int virtio_gpu_resource_id_get(struct virtio_gpu_device *vgdev, uint32_t *resid)
{
	if (virtio_gpu_virglrenderer_workaround) {
		/*
		 * Hack to avoid re-using resource IDs.
		 *
		 * virglrenderer versions up to (and including) 0.7.0
		 * can't deal with that.  virglrenderer commit
		 * "f91a9dd35715 Fix unlinking resources from hash
		 * table." (Feb 2019) fixes the bug.
		 */
		static atomic_t seqno = ATOMIC_INIT(0);
		int handle = atomic_inc_return(&seqno);
		*resid = handle + 1;
	} else {
		int handle = ida_alloc(&vgdev->resource_ida, GFP_KERNEL);

		if (handle < 0)
			return handle;
		*resid = handle + 1;
	}
	return 0;
}

static void virtio_gpu_resource_id_put(struct virtio_gpu_device *vgdev, uint32_t id)
{
	if (!virtio_gpu_virglrenderer_workaround)
		ida_free(&vgdev->resource_ida, id - 1);
}

/*
 * Unmap the backing of a mapped shmem object from the device, with
 * backing_lock held, once the device no longer uses it, and return the maps
 * to free. The control queue is valid until deinit unmaps every object that
 * is still mapped, so an object unmapped later finds no maps.
 */
static dma_addr_t *virtio_gpu_object_unmap_locked(struct virtio_gpu_device *vgdev,
						  struct virtio_gpu_object_shmem *shmem)
{
	struct sg_table *pages = shmem->base.base.sgt;
	dma_addr_t *maps = shmem->maps;
	struct scatterlist *sg;
	int si;

	if (!maps)
		return NULL;
	for_each_sgtable_sg(pages, sg, si) {
		if (si == shmem->nr_maps)
			break;
		virtqueue_unmap_page_attrs(vgdev->ctrlq.vq, maps[si],
					   sg->length, DMA_TO_DEVICE, 0);
	}
	list_del_init(&shmem->mapped);
	shmem->maps = NULL;
	vgdev->backing_bytes -= shmem->base.base.base.size;
	return maps;
}

static void virtio_gpu_object_unmap(struct virtio_gpu_device *vgdev,
				    struct virtio_gpu_object_shmem *shmem)
{
	dma_addr_t *maps;

	spin_lock(&vgdev->backing_lock);
	maps = virtio_gpu_object_unmap_locked(vgdev, shmem);
	spin_unlock(&vgdev->backing_lock);
	kvfree(maps);
}

/*
 * Each object is unmapped under the lock that found it, since a concurrent
 * cleanup may otherwise free it in between.
 */
void virtio_gpu_object_unmap_all(struct virtio_gpu_device *vgdev)
{
	struct virtio_gpu_object_shmem *shmem;
	dma_addr_t *maps;

	for (;;) {
		spin_lock(&vgdev->backing_lock);
		shmem = list_first_entry_or_null(&vgdev->mapped_backing,
						 struct virtio_gpu_object_shmem,
						 mapped);
		maps = shmem ? virtio_gpu_object_unmap_locked(vgdev, shmem) :
			       NULL;
		spin_unlock(&vgdev->backing_lock);
		if (!shmem)
			return;
		kvfree(maps);
	}
}

/*
 * Map the backing of a shmem object through the device's virtio map
 * operations, which give the device addresses of its own for the pages. With
 * VDUSE, the backing then takes room in the bounce region for the object's
 * lifetime, which the virtqueues' buffers share, so backing takes at most
 * half of the largest mapping. The map happens inside a drm_dev_enter()
 * section, so that deinit, which runs once the device is unplugged, finds
 * every mapped object.
 */
static int virtio_gpu_object_map(struct virtio_gpu_device *vgdev,
				 struct virtio_gpu_object_shmem *shmem,
				 struct sg_table *pages,
				 struct virtio_gpu_mem_entry *ents)
{
	size_t size = shmem->base.base.base.size;
	struct virtqueue *vq = vgdev->ctrlq.vq;
	size_t limit;
	struct scatterlist *sg;
	dma_addr_t addr;
	int idx, si, ret = 0;

	if (!drm_dev_enter(vgdev->ddev, &idx))
		return -ENODEV;
	limit = virtio_max_dma_size(vgdev->vdev) / 2;
	shmem->maps = kvmalloc_array(pages->orig_nents, sizeof(*shmem->maps),
				     GFP_KERNEL);
	if (!shmem->maps) {
		ret = -ENOMEM;
		goto out;
	}

	spin_lock(&vgdev->backing_lock);
	if (size > limit || vgdev->backing_bytes > limit - size) {
		ret = -ENOMEM;
	} else {
		vgdev->backing_bytes += size;
		list_add(&shmem->mapped, &vgdev->mapped_backing);
	}
	spin_unlock(&vgdev->backing_lock);
	if (ret) {
		kvfree(shmem->maps);
		shmem->maps = NULL;
		goto out;
	}

	for_each_sgtable_sg(pages, sg, si) {
		addr = virtqueue_map_page_attrs(vq, sg_page(sg), sg->offset,
						sg->length, DMA_TO_DEVICE, 0);
		if (virtqueue_map_mapping_error(vq, addr)) {
			ret = -ENOMEM;
			break;
		}
		shmem->maps[si] = addr;
		ents[si].addr = cpu_to_le64(addr);
		ents[si].length = cpu_to_le32(sg->length);
		ents[si].padding = 0;
	}
	shmem->nr_maps = si;
	if (ret)
		virtio_gpu_object_unmap(vgdev, shmem);
out:
	drm_dev_exit(idx);
	return ret;
}

/* Copy the entries of a mapped object that overlap a range, under the lock. */
static void virtio_gpu_object_sync_locked(struct virtio_gpu_device *vgdev,
					  struct virtio_gpu_object *bo,
					  uint64_t offset, uint64_t size)
{
	struct virtio_gpu_object_shmem *shmem = to_virtio_gpu_shmem(bo);
	struct scatterlist *sg;
	uint64_t start = 0, from, to;
	int si;

	if (!shmem->maps)
		return;
	for_each_sgtable_sg(bo->base.sgt, sg, si) {
		from = max(start, offset);
		to = min(start + sg->length, offset + size);
		if (from < to)
			virtqueue_map_sync_single_range_for_device(
				vgdev->ctrlq.vq, shmem->maps[si], from - start,
				to - from, DMA_TO_DEVICE);
		start += sg->length;
	}
}

/*
 * Copy the bytes of a shmem object's backing from offset to offset + size to
 * the device before a transfer reads them. Through virtio map operations,
 * the device sees copies of the backing pages, and only the entries in the
 * range are copied.
 */
void virtio_gpu_object_sync(struct virtio_gpu_device *vgdev,
			    struct virtio_gpu_object *bo, uint64_t offset,
			    uint64_t size)
{
	if (!virtio_gpu_is_shmem(bo))
		return;
	if (!vgdev->vdev->map) {
		if (!virtio_has_dma_quirk(vgdev->vdev))
			dma_sync_sgtable_for_device(vgdev->vdev->dev.parent,
						    bo->base.sgt,
						    DMA_TO_DEVICE);
		return;
	}
	spin_lock(&vgdev->backing_lock);
	virtio_gpu_object_sync_locked(vgdev, bo, offset, size);
	spin_unlock(&vgdev->backing_lock);
}

/*
 * The panic handler syncs as virtio_gpu_object_sync() does, but skips the
 * sync when another CPU, which the panic may have stopped, holds the lock.
 */
void virtio_gpu_panic_object_sync(struct virtio_gpu_device *vgdev,
				  struct virtio_gpu_object *bo,
				  uint64_t offset, uint64_t size)
{
	if (!virtio_gpu_is_shmem(bo))
		return;
	if (!vgdev->vdev->map) {
		virtio_gpu_object_sync(vgdev, bo, offset, size);
		return;
	}
	if (!spin_trylock(&vgdev->backing_lock))
		return;
	virtio_gpu_object_sync_locked(vgdev, bo, offset, size);
	spin_unlock(&vgdev->backing_lock);
}

void virtio_gpu_cleanup_object(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
	if (virtio_gpu_is_shmem(bo)) {
		virtio_gpu_object_unmap(vgdev, to_virtio_gpu_shmem(bo));
		drm_gem_shmem_free(&bo->base);
	} else if (virtio_gpu_is_vram(bo)) {
		struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);

		spin_lock(&vgdev->host_visible_lock);
		if (drm_mm_node_allocated(&vram->vram_node))
			drm_mm_remove_node(&vram->vram_node);

		spin_unlock(&vgdev->host_visible_lock);

		drm_gem_free_mmap_offset(&vram->base.base.base);
		drm_gem_object_release(&vram->base.base.base);
		kfree(vram);
	} else {
		drm_gem_object_release(&bo->base.base);
		kfree(bo);
	}
}

static void virtio_gpu_free_object(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;

	if (bo->created) {
		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
		/* completion handler calls virtio_gpu_cleanup_object() */
		return;
	}
	virtio_gpu_cleanup_object(bo);
}

int virtio_gpu_detach_object_fenced(struct virtio_gpu_object *bo)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	struct virtio_gpu_fence *fence;

	if (!bo->attached)
		return 0;

	fence = virtio_gpu_fence_alloc(vgdev, vgdev->fence_drv.context, 0);
	if (!fence)
		return -ENOMEM;

	virtio_gpu_object_detach(vgdev, bo, fence);
	virtio_gpu_notify(vgdev);

	dma_fence_wait(&fence->f, false);
	dma_fence_put(&fence->f);

	return 0;
}

static const struct drm_gem_object_funcs virtio_gpu_shmem_funcs = {
	.free = virtio_gpu_free_object,
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,
	.print_info = drm_gem_shmem_object_print_info,
	.export = virtgpu_gem_prime_export,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

bool virtio_gpu_is_shmem(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_shmem_funcs;
}

struct drm_gem_object *virtio_gpu_create_object(struct drm_device *dev,
						size_t size)
{
	struct virtio_gpu_object_shmem *shmem;
	struct drm_gem_shmem_object *dshmem;

	shmem = kzalloc_obj(*shmem);
	if (!shmem)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&shmem->mapped);
	dshmem = &shmem->base.base;
	dshmem->base.funcs = &virtio_gpu_shmem_funcs;
	return &dshmem->base;
}

static int virtio_gpu_object_shmem_init(struct virtio_gpu_device *vgdev,
					struct virtio_gpu_object *bo,
					struct virtio_gpu_mem_entry **ents,
					unsigned int *nents)
{
	bool use_dma_api = !virtio_has_dma_quirk(vgdev->vdev);
	struct scatterlist *sg;
	struct sg_table *pages;
	int si, ret;

	pages = drm_gem_shmem_get_pages_sgt(&bo->base);
	if (IS_ERR(pages))
		return PTR_ERR(pages);

	if (use_dma_api && !vgdev->vdev->map)
		*nents = pages->nents;
	else
		*nents = pages->orig_nents;

	*ents = kvmalloc_objs(struct virtio_gpu_mem_entry, *nents);
	if (!(*ents)) {
		DRM_ERROR("failed to allocate ent list\n");
		return -ENOMEM;
	}

	if (vgdev->vdev->map) {
		ret = virtio_gpu_object_map(vgdev, to_virtio_gpu_shmem(bo),
					    pages, *ents);
		if (ret) {
			kvfree(*ents);
			*ents = NULL;
		}
		return ret;
	}

	if (use_dma_api) {
		for_each_sgtable_dma_sg(pages, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_dma_address(sg));
			(*ents)[si].length = cpu_to_le32(sg_dma_len(sg));
			(*ents)[si].padding = 0;
		}
	} else {
		for_each_sgtable_sg(pages, sg, si) {
			(*ents)[si].addr = cpu_to_le64(sg_phys(sg));
			(*ents)[si].length = cpu_to_le32(sg->length);
			(*ents)[si].padding = 0;
		}
	}

	return 0;
}

int virtio_gpu_object_create(struct virtio_gpu_device *vgdev,
			     struct virtio_gpu_object_params *params,
			     struct virtio_gpu_object **bo_ptr,
			     struct virtio_gpu_fence *fence)
{
	struct virtio_gpu_object_array *objs = NULL;
	struct drm_gem_shmem_object *shmem_obj;
	struct virtio_gpu_object *bo;
	struct virtio_gpu_mem_entry *ents = NULL;
	unsigned int nents;
	int ret;

	*bo_ptr = NULL;

	params->size = roundup(params->size, PAGE_SIZE);
	shmem_obj = drm_gem_shmem_create(vgdev->ddev, params->size);
	if (IS_ERR(shmem_obj))
		return PTR_ERR(shmem_obj);
	bo = gem_to_virtio_gpu_obj(&shmem_obj->base);

	ret = virtio_gpu_resource_id_get(vgdev, &bo->hw_res_handle);
	if (ret < 0)
		goto err_free_gem;

	bo->dumb = params->dumb;

	ret = virtio_gpu_object_shmem_init(vgdev, bo, &ents, &nents);
	if (ret != 0)
		goto err_put_id;

	if (fence) {
		ret = -ENOMEM;
		objs = virtio_gpu_array_alloc(1);
		if (!objs)
			goto err_free_entry;
		virtio_gpu_array_add_obj(objs, &bo->base.base);

		ret = virtio_gpu_array_lock_resv(objs);
		if (ret != 0)
			goto err_put_objs;
	}

	if (params->blob) {
		if (params->blob_mem == VIRTGPU_BLOB_MEM_GUEST)
			bo->guest_blob = true;

		virtio_gpu_cmd_resource_create_blob(vgdev, bo, params,
						    ents, nents);
	} else if (params->virgl) {
		virtio_gpu_cmd_resource_create_3d(vgdev, bo, params,
						  objs, fence);
		virtio_gpu_object_attach(vgdev, bo, ents, nents);
	} else {
		virtio_gpu_cmd_create_resource(vgdev, bo, params,
					       objs, fence);
		virtio_gpu_object_attach(vgdev, bo, ents, nents);
	}

	*bo_ptr = bo;
	return 0;

err_put_objs:
	virtio_gpu_array_put_free(objs);
err_free_entry:
	kvfree(ents);
	virtio_gpu_object_unmap(vgdev, to_virtio_gpu_shmem(bo));
err_put_id:
	virtio_gpu_resource_id_put(vgdev, bo->hw_res_handle);
err_free_gem:
	drm_gem_shmem_free(shmem_obj);
	return ret;
}
