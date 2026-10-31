// SPDX-License-Identifier: GPL-2.0
#include "virtgpu_drv.h"

#include <linux/dma-mapping.h>

static DEFINE_MUTEX(map_lock);

static void virtio_gpu_vram_free(struct drm_gem_object *obj)
{
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	bool unmap;

	if (bo->created) {
		spin_lock(&vgdev->host_visible_lock);
		unmap = drm_mm_node_allocated(&vram->vram_node);
		spin_unlock(&vgdev->host_visible_lock);

		if (unmap)
			virtio_gpu_cmd_unmap(vgdev, bo);

		virtio_gpu_cmd_unref_resource(vgdev, bo);
		virtio_gpu_notify(vgdev);
		return;
	}
}

static const struct vm_operations_struct virtio_gpu_vram_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/*
 * A fault on a blob of a region without a physical address goes to the
 * transport with the blob's offset in the region. The mapping keeps the
 * blob's fake offset in vm_pgoff. Until deinit closes the region, the
 * transport stays bound. It may release the fault's lock and ask for a
 * retry, after which neither the VMA nor the blob is used here.
 */
static vm_fault_t virtio_gpu_vram_fault(struct vm_fault *vmf)
{
	struct drm_gem_object *obj = vmf->vma->vm_private_data;
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object_vram *vram =
		to_virtio_gpu_vram(gem_to_virtio_gpu_obj(obj));
	u64 offset;
	vm_fault_t ret;

	offset = (u64)(vmf->pgoff - drm_vma_node_start(&obj->vma_node)) <<
		 PAGE_SHIFT;
	if (offset >= vram->vram_node.size)
		return VM_FAULT_SIGBUS;

	down_read(&vgdev->host_visible_fault_sem);
	if (vgdev->host_visible_closed)
		ret = VM_FAULT_SIGBUS;
	else
		ret = virtio_shm_fault(vgdev->vdev,
				       VIRTIO_GPU_SHM_ID_HOST_VISIBLE,
				       vram->vram_node.start + offset, vmf);
	up_read(&vgdev->host_visible_fault_sem);
	return ret;
}

static const struct vm_operations_struct virtio_gpu_vram_fault_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
	.fault = virtio_gpu_vram_fault,
};

/*
 * Map a blob of a region without a physical address. The transport inserts
 * its pages on faults with the caching type that it chooses, so map_info is
 * ignored. The mapping is shared and VM_PFNMAP, so that no process can pin
 * the pages past an invalidation.
 */
static int virtio_gpu_vram_mmap_fault(struct drm_gem_object *obj,
				      struct vm_area_struct *vma)
{
	struct virtio_gpu_object_vram *vram =
		to_virtio_gpu_vram(gem_to_virtio_gpu_obj(obj));
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	unsigned long pgoff, vm_end;

	if (is_cow_mapping(vma->vm_flags))
		return -EINVAL;

	pgoff = vma->vm_pgoff - drm_vma_node_start(&obj->vma_node);
	if (check_add_overflow(pgoff << PAGE_SHIFT, vm_size, &vm_end) ||
	    vm_end > vram->vram_node.size)
		return -EINVAL;

	vm_flags_set(vma, VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_ops = &virtio_gpu_vram_fault_vm_ops;
	return 0;
}

static int virtio_gpu_vram_mmap(struct drm_gem_object *obj,
				struct vm_area_struct *vma)
{
	int ret;
	struct virtio_gpu_device *vgdev = obj->dev->dev_private;
	struct virtio_gpu_object *bo = gem_to_virtio_gpu_obj(obj);
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	unsigned long vm_end;

	if (!(bo->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
		return -EINVAL;

	virtio_gpu_vram_map_deferred(vram);

	if (vram->map_state == STATE_INITIALIZING)
		virtio_gpu_notify(vgdev);

	wait_event(vgdev->resp_wq, vram->map_state != STATE_INITIALIZING);
	if (vram->map_state != STATE_OK)
		return -EINVAL;

	if (vgdev->host_visible_region.no_phys)
		return virtio_gpu_vram_mmap_fault(obj, vma);

	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	vm_flags_set(vma, VM_MIXEDMAP | VM_DONTEXPAND);
	vma->vm_page_prot = vm_get_page_prot(vma->vm_flags);
	vma->vm_page_prot = pgprot_decrypted(vma->vm_page_prot);
	vma->vm_ops = &virtio_gpu_vram_vm_ops;

	if (vram->map_info == VIRTIO_GPU_MAP_CACHE_WC)
		vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	else if (vram->map_info == VIRTIO_GPU_MAP_CACHE_UNCACHED)
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	if (check_add_overflow(vma->vm_pgoff << PAGE_SHIFT, vm_size, &vm_end))
		return -EINVAL;

	if (vm_end > vram->vram_node.size)
		return -EINVAL;

	ret = io_remap_pfn_range(vma, vma->vm_start,
				 (vram->vram_node.start >> PAGE_SHIFT) + vma->vm_pgoff,
				 vm_size, vma->vm_page_prot);
	return ret;
}

struct sg_table *virtio_gpu_vram_map_dma_buf(struct virtio_gpu_object *bo,
					     struct device *dev,
					     enum dma_data_direction dir)
{
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);
	struct sg_table *sgt;
	dma_addr_t addr;
	int ret;

	sgt = kzalloc_obj(*sgt);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	if (!(bo->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE)) {
		// Virtio devices can access the dma-buf via its UUID. Return a stub
		// sg_table so the dma-buf API still works.
		if (!is_virtio_device(dev) || !vgdev->has_resource_assign_uuid) {
			ret = -EIO;
			goto out;
		}
		return sgt;
	}

	/* Another device cannot reach a region without a physical address. */
	if (vgdev->host_visible_region.no_phys) {
		ret = -EIO;
		goto out;
	}

	ret = sg_alloc_table(sgt, 1, GFP_KERNEL);
	if (ret)
		goto out;

	addr = dma_map_resource(dev, vram->vram_node.start,
				vram->vram_node.size, dir,
				DMA_ATTR_SKIP_CPU_SYNC);
	ret = dma_mapping_error(dev, addr);
	if (ret)
		goto out;

	sg_set_page(sgt->sgl, NULL, vram->vram_node.size, 0);
	sg_dma_address(sgt->sgl) = addr;
	sg_dma_len(sgt->sgl) = vram->vram_node.size;

	return sgt;
out:
	sg_free_table(sgt);
	kfree(sgt);
	return ERR_PTR(ret);
}

void virtio_gpu_vram_unmap_dma_buf(struct device *dev,
				   struct sg_table *sgt,
				   enum dma_data_direction dir)
{
	if (sgt->nents) {
		dma_unmap_resource(dev, sg_dma_address(sgt->sgl),
				   sg_dma_len(sgt->sgl), dir,
				   DMA_ATTR_SKIP_CPU_SYNC);
	}
	sg_free_table(sgt);
	kfree(sgt);
}

static const struct drm_gem_object_funcs virtio_gpu_vram_funcs = {
	.open = virtio_gpu_gem_object_open,
	.close = virtio_gpu_gem_object_close,
	.free = virtio_gpu_vram_free,
	.mmap = virtio_gpu_vram_mmap,
	.export = virtgpu_gem_prime_export,
};

bool virtio_gpu_is_vram(struct virtio_gpu_object *bo)
{
	return bo->base.base.funcs == &virtio_gpu_vram_funcs;
}

static int virtio_gpu_vram_map(struct virtio_gpu_object *bo)
{
	int ret;
	uint64_t offset;
	struct virtio_gpu_object_array *objs;
	struct virtio_gpu_device *vgdev = bo->base.base.dev->dev_private;
	struct virtio_gpu_object_vram *vram = to_virtio_gpu_vram(bo);

	if (!vgdev->has_host_visible)
		return -EINVAL;

	spin_lock(&vgdev->host_visible_lock);
	ret = drm_mm_insert_node(&vgdev->host_visible_mm, &vram->vram_node,
				 bo->base.base.size);
	spin_unlock(&vgdev->host_visible_lock);

	if (ret)
		return ret;

	objs = virtio_gpu_array_alloc(1);
	if (!objs) {
		ret = -ENOMEM;
		goto err_remove_node;
	}

	virtio_gpu_array_add_obj(objs, &bo->base.base);
	/*TODO: Add an error checking helper function in drm_mm.h */
	offset = vram->vram_node.start - vgdev->host_visible_region.addr;

	ret = virtio_gpu_cmd_map(vgdev, objs, offset);
	if (ret) {
		virtio_gpu_array_put_free(objs);
		goto err_remove_node;
	}

	return 0;

err_remove_node:
	spin_lock(&vgdev->host_visible_lock);
	drm_mm_remove_node(&vram->vram_node);
	spin_unlock(&vgdev->host_visible_lock);
	return ret;
}

int virtio_gpu_vram_create(struct virtio_gpu_device *vgdev,
			   struct virtio_gpu_object_params *params,
			   struct virtio_gpu_object **bo_ptr)
{
	struct drm_gem_object *obj;
	struct virtio_gpu_object_vram *vram;
	int ret;

	vram = kzalloc_obj(*vram);
	if (!vram)
		return -ENOMEM;

	obj = &vram->base.base.base;
	obj->funcs = &virtio_gpu_vram_funcs;

	params->size = PAGE_ALIGN(params->size);
	drm_gem_private_object_init(vgdev->ddev, obj, params->size);

	/* Create fake offset */
	ret = drm_gem_create_mmap_offset(obj);
	if (ret) {
		kfree(vram);
		return ret;
	}

	ret = virtio_gpu_resource_id_get(vgdev, &vram->base.hw_res_handle);
	if (ret) {
		kfree(vram);
		return ret;
	}

	virtio_gpu_cmd_resource_create_blob(vgdev, &vram->base, params, NULL,
					    0);
	if (!(params->blob_hints & DRM_VIRTGPU_BLOB_FLAG_HINT_DEFER_MAPPING)) {
		if (params->blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE) {
			ret = virtio_gpu_vram_map(&vram->base);
			if (ret) {
				virtio_gpu_vram_free(obj);
				return ret;
			}
		}
	}

	*bo_ptr = &vram->base;
	return 0;
}

/*
 * Zap the mappings of the blobs whose placements overlap an invalidated
 * range of the host-visible region. Each blob is found under the lock that
 * protects the placements, and a blob whose last reference is gone has no
 * mapping left. Dropping a reference can free the blob, which sends the
 * device a command, so the references are dropped from a work item instead
 * of from the transport's call.
 */
void virtio_gpu_vram_invalidate(struct virtio_device *vdev, u8 id,
				u64 offset, u64 len)
{
	struct drm_device *dev = vdev->priv;
	struct virtio_gpu_device *vgdev = dev->dev_private;
	u64 pos = offset, end = offset + len;

	if (id != VIRTIO_GPU_SHM_ID_HOST_VISIBLE)
		return;

	while (pos < end) {
		struct virtio_gpu_object_array *objs;
		struct drm_gem_object *obj = NULL;
		struct drm_mm_node *node;

		objs = virtio_gpu_array_alloc(1);
		if (!objs) {
			/* Without memory, every mapping of the device is zapped. */
			unmap_mapping_range(dev->anon_inode->i_mapping, 0, 0, 1);
			return;
		}

		spin_lock(&vgdev->host_visible_lock);
		drm_mm_for_each_node_in_range(node, &vgdev->host_visible_mm,
					      pos, end) {
			struct virtio_gpu_object_vram *vram =
				container_of(node, struct virtio_gpu_object_vram,
					     vram_node);

			pos = node->start + node->size;
			if (kref_get_unless_zero(&vram->base.base.base.refcount)) {
				obj = &vram->base.base.base;
				break;
			}
		}
		spin_unlock(&vgdev->host_visible_lock);

		if (!obj) {
			kfree(objs);
			return;
		}

		drm_vma_node_unmap(&obj->vma_node, dev->anon_inode->i_mapping);
		objs->objs[0] = obj;
		objs->nents = 1;
		virtio_gpu_array_put_free_delayed(vgdev, objs);
	}
}

void virtio_gpu_vram_map_deferred(struct virtio_gpu_object_vram *vram)
{
	if (!(vram->base.blob_flags & VIRTGPU_BLOB_FLAG_USE_MAPPABLE))
		return;

	mutex_lock(&map_lock);
	if (!drm_mm_node_allocated(&vram->vram_node))
		virtio_gpu_vram_map(&vram->base);
	mutex_unlock(&map_lock);
}
