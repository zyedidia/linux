// SPDX-License-Identifier: GPL-2.0
/*
 * DRM native context for amdgpu, a port of virglrenderer's amdgpu renderer.
 * The ccmds of a context run as ioctls of its DRM file on amdgpu's render
 * node, in the worker of its device, which has adopted an mm of its own, so
 * that the ioctls read and write their arguments in that mm's memory.
 */

#define pr_fmt(fmt) "vduse-guest-gpu: " fmt

#include <drm/amd/amdgpu_gem_pages.h>
#include <drm/amdgpu_drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/bitops.h>
#include <linux/ktime.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include "gpu.h"

/* Virglrenderer's context type for amdgpu in the DRM capset. */
#define VDUSE_GUEST_GPU_CONTEXT_AMDGPU 2

/* The largest result of a QUERY_INFO ccmd, as in virglrenderer. */
#define VDUSE_GUEST_GPU_MAX_QUERY SZ_1M

/*
 * The most GEM_NEW requests without a blob that a context may have, and that
 * the device may have.
 */
#define VDUSE_GUEST_GPU_MAX_GEM_REQS 4096
#define VDUSE_GUEST_GPU_MAX_DEVICE_GEM_REQS 65536

/* The most times that an ioctl runs while it fails with -EAGAIN or -EINTR. */
#define VDUSE_GUEST_GPU_IOCTL_TRIES 100

#define VDUSE_GUEST_GPU_MAX_CHUNKS 128

/* Libdrm's bound on a fence's ring. */
#define VDUSE_GUEST_GPU_MAX_RINGS 8

/* The entries of libdrm's amdgpu.ids that the backend knows. */
static const struct {
	u32 device_id;
	u32 pci_rev;
	const char *name;
} vduse_guest_gpu_amdgpu_names[] = {
	{ 0x164e, 0xd8, "AMD Radeon 610M" },
	{ 0x164e, 0xd9, "AMD Radeon 610M" },
	{ 0x164e, 0xda, "AMD Radeon 610M" },
	{ 0x164e, 0xdb, "AMD Radeon 610M" },
	{ 0x164e, 0xdc, "AMD Radeon 610M" },
};

/*
 * The ccmds of virglrenderer's amdgpu_virtio_proto.h, in the host's byte
 * order. Each starts with a header, and the device writes its response,
 * which starts with a response header, at rsp_off in the context's shared
 * memory buffer.
 */
enum {
	CCMD_QUERY_INFO = 1,
	CCMD_GEM_NEW,
	CCMD_BO_VA_OP,
	CCMD_CS_SUBMIT,
	CCMD_SET_METADATA,
	CCMD_BO_QUERY_INFO,
	CCMD_CREATE_CTX,
	CCMD_RESERVE_VMID,
	CCMD_SET_PSTATE,
	CCMD_CS_QUERY_FENCE_STATUS,
};

struct ccmd_hdr {
	u32 cmd;
	u32 len;
	u32 seqno;
	u32 rsp_off;
};

struct ccmd_rsp {
	u32 len;
	s32 ret;
};

struct ccmd_query_info_req {
	struct ccmd_hdr hdr;
	struct drm_amdgpu_info info;
};

struct ccmd_query_info_rsp {
	struct ccmd_rsp hdr;
	u8 payload[];
};

struct ccmd_gem_new_req {
	struct ccmd_hdr hdr;
	u64 blob_id;
	u64 alloc_size;
	u64 phys_alignment;
	u32 preferred_heap;
	u32 pad;
	u64 flags;
};

#define CCMD_BO_VA_OP_SPARSE_BO BIT(0)

struct ccmd_bo_va_op_req {
	struct ccmd_hdr hdr;
	u64 va;
	u64 vm_map_size;
	u64 flags;
	u64 flags2;
	u64 offset;
	u32 res_id;
	u32 op;
};

struct ccmd_cs_submit_req {
	struct ccmd_hdr hdr;
	u32 ctx_id;
	u32 num_chunks;
	u32 pad;
	u32 ring_idx;
	u8 payload[];
};

/* A chunk's descriptor at the start of a CS_SUBMIT's payload. */
struct ccmd_cs_desc {
	u16 chunk_id;
	u16 length_dw;
	u32 offset;
};

struct ccmd_set_metadata_req {
	struct ccmd_hdr hdr;
	u64 flags;
	u64 tiling_info;
	u32 res_id;
	u32 size_metadata;
	u32 umd_metadata[];
};

struct ccmd_bo_query_info_req {
	struct ccmd_hdr hdr;
	u32 res_id;
	u32 pad;
};

struct ccmd_bo_query_info_rsp {
	struct ccmd_rsp hdr;
	u64 alloc_size;
	u64 phys_alignment;
	u32 preferred_heap;
	u32 pad;
	u64 alloc_flags;
	u64 metadata_flags;
	u64 tiling_info;
	u32 size_metadata;
	u32 umd_metadata[64];
	u32 pad2;
};

#define CCMD_CREATE_CTX_DESTROY BIT(0)

struct ccmd_create_ctx_req {
	struct ccmd_hdr hdr;
	union {
		s32 priority;
		u32 id;
	};
	u32 flags;
};

struct ccmd_create_ctx_rsp {
	struct ccmd_rsp hdr;
	u32 ctx_id;
	u32 pad;
};

#define CCMD_RESERVE_VMID_UNRESERVE BIT(0)

struct ccmd_reserve_vmid_req {
	struct ccmd_hdr hdr;
	u64 flags;
};

struct ccmd_set_pstate_req {
	struct ccmd_hdr hdr;
	u32 ctx_id;
	u32 op;
	u32 flags;
	u32 pad;
};

struct ccmd_set_pstate_rsp {
	struct ccmd_rsp hdr;
	u32 out_flags;
	u32 pad;
};

struct ccmd_cs_query_fence_status_req {
	struct ccmd_hdr hdr;
	u32 ctx_id;
	u32 ip_type;
	u32 ip_instance;
	u32 ring;
	u64 fence;
	u64 timeout_ns;
	u64 flags;
};

struct ccmd_cs_query_fence_status_rsp {
	struct ccmd_rsp hdr;
	u32 expired;
	u32 pad;
};

static_assert(sizeof(struct ccmd_query_info_req) == 48);
static_assert(sizeof(struct ccmd_gem_new_req) == 56);
static_assert(sizeof(struct ccmd_bo_va_op_req) == 64);
static_assert(sizeof(struct ccmd_cs_submit_req) == 32);
static_assert(sizeof(struct ccmd_set_metadata_req) == 40);
static_assert(sizeof(struct ccmd_bo_query_info_req) == 24);
static_assert(sizeof(struct ccmd_bo_query_info_rsp) == 320);
static_assert(sizeof(struct ccmd_create_ctx_req) == 24);
static_assert(sizeof(struct ccmd_create_ctx_rsp) == 16);
static_assert(sizeof(struct ccmd_reserve_vmid_req) == 24);
static_assert(sizeof(struct ccmd_set_pstate_req) == 32);
static_assert(sizeof(struct ccmd_set_pstate_rsp) == 16);
static_assert(sizeof(struct ccmd_cs_query_fence_status_req) == 56);
static_assert(sizeof(struct ccmd_cs_query_fence_status_rsp) == 16);

static struct drm_file *vduse_guest_gpu_drm_file(struct file *file)
{
	return file->private_data;
}

/*
 * Run an ioctl of a DRM file on a copy of arg in the worker's memory, and
 * copy the result back. Pointers within arg must point into that memory. As
 * libdrm's drmIoctl() does, an ioctl that fails with -EAGAIN or -EINTR runs
 * again, as when amdgpu finds a BO evicted during a submission, but only a
 * bounded number of times, since the worker serves every host process.
 */
static int vduse_guest_gpu_ioctl(struct vduse_guest_gpu *gpu,
				 struct file *file, unsigned int cmd,
				 void *arg, size_t size)
{
	void __user *uarg = vduse_guest_gpu_ualloc(gpu, size);
	unsigned int tries = 0;
	long err;

	if (!uarg)
		return -ENOMEM;
	do {
		if (copy_to_user(uarg, arg, size))
			return -EFAULT;
		err = file->f_op->unlocked_ioctl(file, cmd, (unsigned long)uarg);
		cond_resched();
	} while ((err == -EAGAIN || err == -EINTR) &&
		 ++tries < VDUSE_GUEST_GPU_IOCTL_TRIES);
	if (copy_from_user(arg, uarg, size))
		return -EFAULT;
	return err;
}

/* Copy a buffer into the worker's memory. */
static void __user *vduse_guest_gpu_ucopy(struct vduse_guest_gpu *gpu,
					  const void *buf, size_t size)
{
	void __user *ubuf = vduse_guest_gpu_ualloc(gpu, size);

	if (!ubuf || copy_to_user(ubuf, buf, size))
		return NULL;
	return ubuf;
}

/* Run an AMDGPU_INFO query, whose result goes to out. */
static int vduse_guest_gpu_info(struct vduse_guest_gpu *gpu,
				struct file *file,
				struct drm_amdgpu_info *req, void *out,
				u32 size)
{
	void __user *uout = vduse_guest_gpu_ualloc(gpu, size);
	int err;

	if (!uout || clear_user(uout, size))
		return -ENOMEM;
	req->return_pointer = (uintptr_t)uout;
	req->return_size = size;
	err = vduse_guest_gpu_ioctl(gpu, file, DRM_IOCTL_AMDGPU_INFO, req,
				    sizeof(*req));
	if (err)
		return err;
	if (copy_from_user(out, uout, size))
		return -EFAULT;
	return 0;
}

static int vduse_guest_gpu_info_u64(struct vduse_guest_gpu *gpu,
				    struct file *file, u32 query, u64 *value)
{
	struct drm_amdgpu_info req = { .query = query };

	return vduse_guest_gpu_info(gpu, file, &req, value, sizeof(*value));
}

static int vduse_guest_gpu_regs(struct vduse_guest_gpu *gpu, struct file *file,
				u32 offset, u32 count, u32 instance,
				u32 *values)
{
	struct drm_amdgpu_info req = {
		.query = AMDGPU_INFO_READ_MMR_REG,
		.read_mmr_reg = {
			.dword_offset = offset,
			.count = count,
			.instance = instance,
		},
	};

	return vduse_guest_gpu_info(gpu, file, &req, values,
				    count * sizeof(u32));
}

/* Open the render node, which must be one of amdgpu. */
static struct file *vduse_guest_gpu_open(struct vduse_guest_gpu *gpu)
{
	const struct drm_driver *driver;
	struct drm_file *file_priv;
	struct file *file;

	file = filp_open(gpu->render_path, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(file))
		return file;

	if (!S_ISCHR(file_inode(file)->i_mode) ||
	    imajor(file_inode(file)) != DRM_MAJOR)
		goto fail;
	file_priv = vduse_guest_gpu_drm_file(file);
	if (!drm_is_render_client(file_priv))
		goto fail;
	driver = file_priv->minor->dev->driver;
	if (strcmp(driver->name, "amdgpu") || driver->major != 3)
		goto fail;
	return file;

fail:
	filp_close(file, NULL);
	return ERR_PTR(-ENODEV);
}

/*
 * Fill the register values of the capset's GPU information, with the
 * queries that libdrm's amdgpu_query_gpu_info_init() makes.
 */
static int vduse_guest_gpu_info_regs(struct vduse_guest_gpu *gpu,
				     struct file *file,
				     struct vduse_guest_gpu_amdgpu_info *info)
{
	unsigned int i, nse;
	int err;

	if (info->family_id < AMDGPU_FAMILY_AI) {
		nse = min_t(unsigned int, info->num_shader_engines,
			    ARRAY_SIZE(info->backend_disable));
		for (i = 0; i < nse; i++) {
			u32 instance = (i << AMDGPU_INFO_MMR_SE_INDEX_SHIFT) |
				       (AMDGPU_INFO_MMR_SH_INDEX_MASK <<
					AMDGPU_INFO_MMR_SH_INDEX_SHIFT);

			err = vduse_guest_gpu_regs(gpu, file, 0x263d, 1,
						   instance,
						   &info->backend_disable[i]);
			if (err)
				return err;
			info->backend_disable[i] =
				(info->backend_disable[i] >> 16) & 0xff;

			err = vduse_guest_gpu_regs(gpu, file, 0xa0d4, 1,
						   instance,
						   &info->pa_sc_raster_cfg[i]);
			if (err)
				return err;

			if (info->family_id >= AMDGPU_FAMILY_CI) {
				err = vduse_guest_gpu_regs(gpu, file, 0xa0d5, 1,
							   instance,
							   &info->pa_sc_raster_cfg1[i]);
				if (err)
					return err;
			}
		}
	}

	err = vduse_guest_gpu_regs(gpu, file, 0x263e, 1, 0xffffffff,
				   &info->gb_addr_cfg);
	if (err)
		return err;

	if (info->family_id < AMDGPU_FAMILY_AI) {
		err = vduse_guest_gpu_regs(gpu, file, 0x2644, 32, 0xffffffff,
					   info->gb_tile_mode);
		if (err)
			return err;

		if (info->family_id >= AMDGPU_FAMILY_CI) {
			err = vduse_guest_gpu_regs(gpu, file, 0x2664, 16,
						   0xffffffff,
						   info->gb_macro_tile_mode);
			if (err)
				return err;
		}

		err = vduse_guest_gpu_regs(gpu, file, 0x9d8, 1, 0xffffffff,
					   &info->mc_arb_ramcfg);
		if (err)
			return err;
	}

	return 0;
}

/* Fill the amdgpu part of the capset as virglrenderer does through libdrm. */
static int vduse_guest_gpu_caps(struct vduse_guest_gpu *gpu, struct file *file,
				struct vduse_guest_gpu_capset *caps)
{
	struct vduse_guest_gpu_amdgpu_info *info = &caps->amdgpu.gpu_info;
	struct drm_amdgpu_info req = {};
	struct drm_amdgpu_info_device *di;
	const char *name = "AMD Radeon Graphics";
	u32 accel_working = 0;
	u64 va_max;
	unsigned int i;
	int err;

	req.query = AMDGPU_INFO_ACCEL_WORKING;
	err = vduse_guest_gpu_info(gpu, file, &req, &accel_working,
				   sizeof(accel_working));
	if (err)
		return err;
	if (!accel_working)
		return -ENODEV;

	di = kzalloc_obj(*di);
	if (!di)
		return -ENOMEM;

	memset(&req, 0, sizeof(req));
	req.query = AMDGPU_INFO_DEV_INFO;
	err = vduse_guest_gpu_info(gpu, file, &req, di, sizeof(*di));
	if (err)
		goto out;

	info->asic_id = di->device_id;
	info->chip_rev = di->chip_rev;
	info->chip_external_rev = di->external_rev;
	info->family_id = di->family;
	info->ids_flags = di->ids_flags;
	info->max_engine_clk = di->max_engine_clock;
	info->max_memory_clk = di->max_memory_clock;
	info->num_shader_engines = di->num_shader_engines;
	info->num_shader_arrays_per_engine = di->num_shader_arrays_per_engine;
	info->num_hw_gfx_contexts = di->num_hw_gfx_contexts;
	info->rb_pipes = di->num_rb_pipes;
	info->enabled_rb_pipes_mask = di->enabled_rb_pipes_mask;
	info->gpu_counter_freq = di->gpu_counter_freq;
	info->cu_active_number = di->cu_active_number;
	info->cu_ao_mask = di->cu_ao_mask;
	memcpy(info->cu_bitmap, di->cu_bitmap, sizeof(info->cu_bitmap));
	info->vram_type = di->vram_type;
	info->vram_bit_width = di->vram_bit_width;
	info->ce_ram_size = di->ce_ram_size;
	info->vce_harvest_config = di->vce_harvest_config;
	info->pci_rev_id = di->pci_rev;

	err = vduse_guest_gpu_info_regs(gpu, file, info);
	if (err)
		goto out;

	/* Libdrm takes the high bits of 32-bit addresses from its VA manager. */
	if (di->high_va_max)
		va_max = min_t(u64, di->high_va_max,
			       (di->high_va_offset & ~0xffffffffULL) + SZ_4G);
	else
		va_max = min_t(u64, di->virtual_address_max, SZ_4G);
	caps->amdgpu.address32_hi = (va_max - 1) >> 32;
	caps->amdgpu.has_vm_always_valid = 1;
	caps->amdgpu.size_local = di->pte_fragment_size;
	caps->amdgpu.size_remote = di->gart_page_size;

	for (i = 0; i < ARRAY_SIZE(vduse_guest_gpu_amdgpu_names); i++) {
		if (vduse_guest_gpu_amdgpu_names[i].device_id == di->device_id &&
		    vduse_guest_gpu_amdgpu_names[i].pci_rev == di->pci_rev) {
			name = vduse_guest_gpu_amdgpu_names[i].name;
			break;
		}
	}
	strscpy(caps->amdgpu.marketing_name, name);

out:
	kfree(di);
	return err;
}

/* Open the render node and fill the DRM capset from its device. */
int vduse_guest_gpu_amdgpu_capset(struct vduse_guest_gpu *gpu,
				  struct vduse_guest_gpu_capset *caps)
{
	const struct drm_driver *driver;
	struct file *file;
	int err;

	if (!gpu->mm)
		return -ENOMEM;
	file = vduse_guest_gpu_open(gpu);
	if (IS_ERR(file))
		return PTR_ERR(file);

	driver = vduse_guest_gpu_drm_file(file)->minor->dev->driver;
	memset(caps, 0, sizeof(*caps));
	caps->version_major = driver->major;
	caps->version_minor = driver->minor;
	caps->version_patchlevel = driver->patchlevel;
	caps->context_type = VDUSE_GUEST_GPU_CONTEXT_AMDGPU;

	vduse_guest_gpu_ureset(gpu);
	err = vduse_guest_gpu_caps(gpu, file, caps);
	filp_close(file, NULL);
	return err;
}

/*
 * Give a context a DRM file of its own, so that host processes get separate
 * GPU address spaces and handle namespaces, and one timeline for each
 * available ring, numbered from 1 in the order of the IP types as Mesa
 * numbers them. Without a GPU the context gets neither, and its ccmds fail.
 */
void vduse_guest_gpu_amdgpu_ctx_init(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx)
{
	struct drm_amdgpu_info_hw_ip ip;
	unsigned int type, count = 0;
	struct file *file;

	xa_init(&ctx->handles);
	xa_init(&ctx->gem_reqs);
	if (!gpu->mm)
		return;
	file = vduse_guest_gpu_open(gpu);
	if (IS_ERR(file))
		return;

	for (type = 0; type < AMDGPU_HW_IP_NUM; type++) {
		struct drm_amdgpu_info req = {
			.query = AMDGPU_INFO_HW_IP_INFO,
			.query_hw_ip.type = type,
		};

		vduse_guest_gpu_ureset(gpu);
		if (!vduse_guest_gpu_info(gpu, file, &req, &ip, sizeof(ip)))
			count += hweight32(ip.available_rings);
	}

	ctx->timelines = kzalloc_objs(*ctx->timelines, count);
	if (count && !ctx->timelines) {
		filp_close(file, NULL);
		return;
	}
	ctx->nr_timelines = count;
	ctx->file = file;
}

/*
 * Releasing the DRM file releases its handles, syncobjs and amdgpu
 * contexts. The file is put rather than closed, which skips amdgpu's flush.
 * In the worker the flush would wait without a bound for the context's
 * queued jobs, while every host process waits for the worker. The release
 * runs from the delayed fput work, where the scheduler kills the jobs that
 * are still queued, as at a native process's exit. The resources that the
 * context created keep their own references to their BOs.
 */
void vduse_guest_gpu_amdgpu_ctx_fini(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx)
{
	unsigned long index;
	void *req;
	unsigned int i;

	for (i = 0; i < ctx->nr_timelines; i++)
		dma_fence_put(ctx->timelines[i].last);
	kfree(ctx->timelines);
	xa_for_each(&ctx->gem_reqs, index, req)
		kfree(req);
	xa_destroy(&ctx->gem_reqs);
	gpu->nr_gem_reqs -= ctx->nr_gem_reqs;
	ctx->nr_gem_reqs = 0;
	xa_destroy(&ctx->handles);
	if (ctx->file)
		fput(ctx->file);
}

/* Return the fence that a fenced command on a ring of the context waits for. */
struct dma_fence *vduse_guest_gpu_amdgpu_fence(struct vduse_guest_gpu_ctx *ctx,
					       u32 ring_idx)
{
	if (!ring_idx || ring_idx > ctx->nr_timelines)
		return NULL;
	return dma_fence_get(ctx->timelines[ring_idx - 1].last);
}

/* Report the heaps' sizes and usage in the shared memory buffer, as libdrm. */
void vduse_guest_gpu_amdgpu_update_heaps(struct vduse_guest_gpu *gpu,
					 struct vduse_guest_gpu_ctx *ctx)
{
	struct drm_amdgpu_info req = { .query = AMDGPU_INFO_VRAM_GTT };
	struct drm_amdgpu_info_vram_gtt vram_gtt;
	struct vduse_guest_gpu_shmem *shmem;
	u64 vis_vram, vram, gtt;

	if (!ctx->file || !ctx->shmem)
		return;
	shmem = ctx->shmem->vaddr;

	vduse_guest_gpu_ureset(gpu);
	if (vduse_guest_gpu_info(gpu, ctx->file, &req, &vram_gtt,
				 sizeof(vram_gtt)) ||
	    vduse_guest_gpu_info_u64(gpu, ctx->file, AMDGPU_INFO_VIS_VRAM_USAGE,
				     &vis_vram) ||
	    vduse_guest_gpu_info_u64(gpu, ctx->file, AMDGPU_INFO_VRAM_USAGE,
				     &vram) ||
	    vduse_guest_gpu_info_u64(gpu, ctx->file, AMDGPU_INFO_GTT_USAGE,
				     &gtt))
		return;

	shmem->vis_vram.heap_size = vram_gtt.vram_cpu_accessible_size;
	shmem->vis_vram.heap_usage = vis_vram;
	shmem->vis_vram.max_allocation = vram_gtt.vram_cpu_accessible_size;
	shmem->vram.heap_size = vram_gtt.vram_size;
	shmem->vram.heap_usage = vram;
	shmem->vram.max_allocation = vram_gtt.vram_cpu_accessible_size;
	shmem->gtt.heap_size = vram_gtt.gtt_size;
	shmem->gtt.heap_usage = gtt;
	shmem->gtt.max_allocation = vram_gtt.vram_cpu_accessible_size;
}

/* Count an asynchronous error, which Mesa treats as a lost context. */
static void vduse_guest_gpu_async_error(struct vduse_guest_gpu_ctx *ctx)
{
	struct vduse_guest_gpu_shmem *shmem;

	if (!ctx->shmem)
		return;
	shmem = ctx->shmem->vaddr;
	WRITE_ONCE(shmem->async_error, shmem->async_error + 1);
}

/*
 * Allocate the BO of a GEM_NEW request when the blob that names it is
 * created. A mappable blob whose BO allows CPU access is placed in GTT,
 * whatever heap Mesa asked for, unless the vram option is set and Mesa asked
 * for VRAM. It is then created in CPU-visible VRAM, and mapping it keeps it
 * there. The context that creates the blob gets the BO's handle.
 */
int vduse_guest_gpu_amdgpu_create_bo(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx,
				     struct vduse_guest_gpu_res *res,
				     u64 blob_id)
{
	struct ccmd_gem_new_req *req;
	union drm_amdgpu_gem_create args = {};
	struct drm_gem_object *obj;
	u32 domains;
	u64 flags;
	int err;

	req = xa_erase(&ctx->gem_reqs, blob_id);
	if (!req)
		return -ENOENT;
	ctx->nr_gem_reqs--;
	gpu->nr_gem_reqs--;

	err = -ENODEV;
	if (!ctx->file)
		goto out;

	domains = req->preferred_heap;
	flags = req->flags;
	if ((res->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE) &&
	    !(flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)) {
		if (gpu->vram && (domains & AMDGPU_GEM_DOMAIN_VRAM)) {
			domains = AMDGPU_GEM_DOMAIN_VRAM;
			flags |= AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
		} else {
			domains = AMDGPU_GEM_DOMAIN_GTT;
		}
	}

	args.in.bo_size = req->alloc_size;
	args.in.alignment = req->phys_alignment;
	args.in.domains = domains;
	args.in.domain_flags = flags;
	vduse_guest_gpu_ureset(gpu);
	err = vduse_guest_gpu_ioctl(gpu, ctx->file, DRM_IOCTL_AMDGPU_GEM_CREATE,
				    &args, sizeof(args));
	if (err)
		goto out;

	obj = drm_gem_object_lookup(vduse_guest_gpu_drm_file(ctx->file),
				    args.out.handle);
	err = obj ? xa_err(xa_store(&ctx->handles, res->id,
				    xa_mk_value(args.out.handle),
				    GFP_KERNEL)) : -ENOENT;
	if (err) {
		if (obj)
			drm_gem_object_put(obj);
		drm_gem_handle_delete(vduse_guest_gpu_drm_file(ctx->file),
				      args.out.handle);
		goto out;
	}

	res->obj = obj;
	res->bo_domains = domains;
	res->bo_flags = flags;
	vduse_guest_gpu_amdgpu_update_heaps(gpu, ctx);
out:
	if (err)
		vduse_guest_gpu_async_error(ctx);
	kfree(req);
	return err;
}

/*
 * Give a context a handle to a resource's BO, unless it has one. Blob 0 has
 * no BO and needs no handle.
 */
int vduse_guest_gpu_amdgpu_attach(struct vduse_guest_gpu_ctx *ctx,
				  struct vduse_guest_gpu_res *res)
{
	u32 handle;
	int err;

	if (!res->obj || !ctx->file || xa_load(&ctx->handles, res->id))
		return 0;

	err = drm_gem_handle_create(vduse_guest_gpu_drm_file(ctx->file),
				    res->obj, &handle);
	if (err)
		return err;
	err = xa_err(xa_store(&ctx->handles, res->id, xa_mk_value(handle),
			      GFP_KERNEL));
	if (err)
		drm_gem_handle_delete(vduse_guest_gpu_drm_file(ctx->file),
				      handle);
	return err;
}

void vduse_guest_gpu_amdgpu_detach(struct vduse_guest_gpu_ctx *ctx,
				   u32 res_id)
{
	void *entry = xa_erase(&ctx->handles, res_id);

	if (entry && ctx->file)
		drm_gem_handle_delete(vduse_guest_gpu_drm_file(ctx->file),
				      xa_to_value(entry));
}

/*
 * Place a mappable BO in GTT and get its pages, with the BO reserved. A BO
 * without CPU access cannot be mapped, as natively. The pages stay valid
 * until the BO moves, which calls its move notification first.
 */
int vduse_guest_gpu_amdgpu_pages(struct vduse_guest_gpu_res *res,
				 struct page ***pages)
{
	unsigned long npages;
	int err;

	if (!res->obj || res->bo_domains != AMDGPU_GEM_DOMAIN_GTT ||
	    (res->bo_flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS))
		return -EINVAL;

	err = amdgpu_gem_gtt_pages(res->obj, pages, &npages);
	if (err)
		return err;
	return npages < res->size >> PAGE_SHIFT ? -EINVAL : 0;
}

/* Set the function that a move of the reserved BO calls first, or NULL. */
void vduse_guest_gpu_amdgpu_set_notify(struct vduse_guest_gpu_res *res,
				       void (*fn)(void *priv))
{
	amdgpu_gem_set_move_notify(res->obj, fn, res);
}

/*
 * Place a mappable BO in CPU-visible VRAM, with the BO reserved, and call fn
 * with the offset in BAR0 and the size of each of its ranges, in order.
 */
int vduse_guest_gpu_amdgpu_vram(struct vduse_guest_gpu_res *res,
				int (*fn)(void *arg, u64 offset, u64 size),
				void *arg)
{
	if (!res->obj || res->bo_domains != AMDGPU_GEM_DOMAIN_VRAM ||
	    (res->bo_flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS))
		return -EINVAL;
	return amdgpu_gem_vram_ranges(res->obj, fn, arg);
}

/* Register BAR0 of the BO's device, through which the host maps VRAM. */
int vduse_guest_gpu_amdgpu_reg_bar(struct vduse_guest_gpu_res *res, u32 *file)
{
	return vduse_guest_shm_reg_bar(res->gpu->dev,
				       to_pci_dev(res->obj->dev->dev), 0, file);
}

/* Pin the reserved BO where it is placed, for its lifetime. */
int vduse_guest_gpu_amdgpu_pin(struct vduse_guest_gpu_res *res)
{
	return amdgpu_gem_pin_placed(res->obj);
}

/* Return a resource's GEM handle in the context, or 0. */
static u32 vduse_guest_gpu_handle(struct vduse_guest_gpu_ctx *ctx, u32 res_id)
{
	void *entry = xa_load(&ctx->handles, res_id);

	return entry ? xa_to_value(entry) : 0;
}

/*
 * Start the response of a ccmd, len bytes, which must fit in the response
 * memory of the context's shared memory buffer at the ccmd's rsp_off. The
 * response is built in a buffer of the device's own and copied once the
 * ccmd has run.
 */
static void *vduse_guest_gpu_rsp(struct vduse_guest_gpu_ctx *ctx,
				 const struct ccmd_hdr *hdr, size_t len)
{
	struct ccmd_rsp *rsp;
	size_t room;

	if (!ctx->shmem)
		return NULL;
	room = ctx->shmem->size - sizeof(struct vduse_guest_gpu_shmem);
	if (hdr->rsp_off > room || len > room - hdr->rsp_off)
		return NULL;

	rsp = kvzalloc(len, GFP_KERNEL);
	if (!rsp)
		return NULL;
	rsp->len = len;
	kvfree(ctx->rsp);
	ctx->rsp = rsp;
	return rsp;
}

static int vduse_guest_gpu_query_info(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_ctx *ctx,
				      void *buf, bool device_timeline)
{
	struct ccmd_query_info_req *req = buf;
	struct ccmd_query_info_rsp *rsp;
	u32 size = req->info.return_size;
	struct drm_amdgpu_info info;
	void __user *uout;

	if (size > VDUSE_GUEST_GPU_MAX_QUERY)
		return -EINVAL;
	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp) + size);
	if (!rsp)
		return -ENOMEM;

	uout = vduse_guest_gpu_ualloc(gpu, size);
	if (!uout || clear_user(uout, size))
		return -ENOMEM;
	info = req->info;
	info.return_pointer = (uintptr_t)uout;
	rsp->hdr.ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
					     DRM_IOCTL_AMDGPU_INFO, &info,
					     sizeof(info));
	if (copy_from_user(rsp->payload, uout, size))
		return -EFAULT;
	return 0;
}

/*
 * Record a GEM_NEW request until the blob that names its blob id. The
 * requests hold guest memory, so their number is bounded for each context
 * and for the device.
 */
static int vduse_guest_gpu_gem_new(struct vduse_guest_gpu *gpu,
				   struct vduse_guest_gpu_ctx *ctx,
				   void *buf, bool device_timeline)
{
	struct ccmd_gem_new_req *req = buf, *copy;
	int err;

	if (req->pad || !req->blob_id || req->blob_id > U32_MAX ||
	    ctx->nr_gem_reqs == VDUSE_GUEST_GPU_MAX_GEM_REQS ||
	    gpu->nr_gem_reqs == VDUSE_GUEST_GPU_MAX_DEVICE_GEM_REQS) {
		err = -EINVAL;
		goto fail;
	}

	copy = kmemdup(req, sizeof(*req), GFP_KERNEL);
	if (!copy) {
		err = -ENOMEM;
		goto fail;
	}
	err = xa_insert(&ctx->gem_reqs, req->blob_id, copy, GFP_KERNEL);
	if (err) {
		kfree(copy);
		goto fail;
	}
	ctx->nr_gem_reqs++;
	gpu->nr_gem_reqs++;
	return 0;

fail:
	vduse_guest_gpu_async_error(ctx);
	return err;
}

static int vduse_guest_gpu_bo_va_op(struct vduse_guest_gpu *gpu,
				    struct vduse_guest_gpu_ctx *ctx,
				    void *buf, bool device_timeline)
{
	struct ccmd_bo_va_op_req *req = buf;
	struct drm_amdgpu_gem_va va = {};
	struct ccmd_rsp *rsp;

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->flags2 & ~CCMD_BO_VA_OP_SPARSE_BO) {
		rsp->ret = -EINVAL;
		return -EINVAL;
	}
	/*
	 * An operation on a BO that is gone fails without stopping the
	 * stream, so that Mesa's next sync still gets its sequence number.
	 */
	if (!(req->flags2 & CCMD_BO_VA_OP_SPARSE_BO)) {
		va.handle = vduse_guest_gpu_handle(ctx, req->res_id);
		if (!va.handle) {
			rsp->ret = -EINVAL;
			return 0;
		}
	}

	va.operation = req->op;
	va.flags = req->flags;
	va.va_address = req->va;
	va.offset_in_bo = req->offset;
	va.map_size = req->vm_map_size;
	rsp->ret = vduse_guest_gpu_ioctl(gpu, ctx->file, DRM_IOCTL_AMDGPU_GEM_VA,
					 &va, sizeof(va));
	if (rsp->ret)
		vduse_guest_gpu_async_error(ctx);
	return 0;
}

static int vduse_guest_gpu_set_metadata(struct vduse_guest_gpu *gpu,
					struct vduse_guest_gpu_ctx *ctx,
					void *buf, bool device_timeline)
{
	struct ccmd_set_metadata_req *req = buf;
	struct drm_amdgpu_gem_metadata args = {};
	struct ccmd_rsp *rsp;

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->size_metadata > sizeof(args.data.data) ||
	    size_add(offsetof(struct ccmd_set_metadata_req, umd_metadata),
		     req->size_metadata) > req->hdr.len) {
		rsp->ret = -EINVAL;
		return -EINVAL;
	}
	/* Setting the metadata of a BO that is gone fails like BO_VA_OP. */
	args.handle = vduse_guest_gpu_handle(ctx, req->res_id);
	if (!args.handle) {
		rsp->ret = -EINVAL;
		return 0;
	}

	args.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
	args.data.flags = req->flags;
	args.data.tiling_info = req->tiling_info;
	args.data.data_size_bytes = req->size_metadata;
	memcpy(args.data.data, req->umd_metadata, req->size_metadata);
	rsp->ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
					 DRM_IOCTL_AMDGPU_GEM_METADATA, &args,
					 sizeof(args));
	return 0;
}

static int vduse_guest_gpu_bo_query_info(struct vduse_guest_gpu *gpu,
					 struct vduse_guest_gpu_ctx *ctx,
					 void *buf, bool device_timeline)
{
	struct ccmd_bo_query_info_req *req = buf;
	struct drm_amdgpu_gem_metadata metadata = {};
	struct drm_amdgpu_gem_create_in info = {};
	struct ccmd_bo_query_info_rsp *rsp;
	struct drm_amdgpu_gem_op op = {};
	void __user *uinfo;
	int ret;

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->pad) {
		rsp->hdr.ret = -EINVAL;
		return -EINVAL;
	}
	metadata.handle = vduse_guest_gpu_handle(ctx, req->res_id);
	if (!metadata.handle) {
		rsp->hdr.ret = -EINVAL;
		return 0;
	}

	metadata.op = AMDGPU_GEM_METADATA_OP_GET_METADATA;
	ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
				    DRM_IOCTL_AMDGPU_GEM_METADATA, &metadata,
				    sizeof(metadata));
	if (!ret && metadata.data.data_size_bytes > sizeof(rsp->umd_metadata))
		ret = -EINVAL;
	if (ret) {
		rsp->hdr.ret = ret;
		return 0;
	}

	uinfo = vduse_guest_gpu_ucopy(gpu, &info, sizeof(info));
	if (!uinfo)
		return -ENOMEM;
	op.handle = metadata.handle;
	op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
	op.value = (uintptr_t)uinfo;
	ret = vduse_guest_gpu_ioctl(gpu, ctx->file, DRM_IOCTL_AMDGPU_GEM_OP,
				    &op, sizeof(op));
	if (!ret && copy_from_user(&info, uinfo, sizeof(info)))
		ret = -EFAULT;
	rsp->hdr.ret = ret;
	if (ret)
		return 0;

	rsp->alloc_size = info.bo_size;
	rsp->phys_alignment = info.alignment;
	rsp->preferred_heap = info.domains;
	rsp->alloc_flags = info.domain_flags;
	rsp->metadata_flags = metadata.data.flags;
	rsp->tiling_info = metadata.data.tiling_info;
	rsp->size_metadata = metadata.data.data_size_bytes;
	memcpy(rsp->umd_metadata, metadata.data.data,
	       metadata.data.data_size_bytes);
	return 0;
}

/*
 * The worker may raise the priority of an amdgpu context, which an
 * unprivileged host process may not, so it creates contexts of at most
 * normal priority.
 */
static int vduse_guest_gpu_create_ctx(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_ctx *ctx,
				      void *buf, bool device_timeline)
{
	struct ccmd_create_ctx_req *req = buf;
	struct ccmd_create_ctx_rsp *rsp;
	union drm_amdgpu_ctx args = {};

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->flags & ~CCMD_CREATE_CTX_DESTROY) {
		rsp->hdr.ret = -EINVAL;
		return -EINVAL;
	}

	if (req->flags & CCMD_CREATE_CTX_DESTROY) {
		args.in.op = AMDGPU_CTX_OP_FREE_CTX;
		args.in.ctx_id = req->id;
		rsp->hdr.ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
						     DRM_IOCTL_AMDGPU_CTX, &args,
						     sizeof(args));
		return 0;
	}

	if (req->priority > AMDGPU_CTX_PRIORITY_NORMAL) {
		rsp->hdr.ret = -EACCES;
		return 0;
	}
	args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
	args.in.priority = req->priority;
	rsp->hdr.ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
					     DRM_IOCTL_AMDGPU_CTX, &args,
					     sizeof(args));
	if (!rsp->hdr.ret)
		rsp->ctx_id = args.out.alloc.ctx_id;
	return 0;
}

static int vduse_guest_gpu_reserve_vmid(struct vduse_guest_gpu *gpu,
					struct vduse_guest_gpu_ctx *ctx,
					void *buf, bool device_timeline)
{
	struct ccmd_reserve_vmid_req *req = buf;
	union drm_amdgpu_vm args = {};
	struct ccmd_rsp *rsp;

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->flags & ~CCMD_RESERVE_VMID_UNRESERVE) {
		rsp->ret = -EINVAL;
		return -EINVAL;
	}

	args.in.op = (req->flags & CCMD_RESERVE_VMID_UNRESERVE) ?
		     AMDGPU_VM_OP_UNRESERVE_VMID : AMDGPU_VM_OP_RESERVE_VMID;
	rsp->ret = vduse_guest_gpu_ioctl(gpu, ctx->file, DRM_IOCTL_AMDGPU_VM,
					 &args, sizeof(args));
	return 0;
}

/* Only the stable pstate operations go to the amdgpu context ioctl. */
static int vduse_guest_gpu_set_pstate(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_ctx *ctx,
				      void *buf, bool device_timeline)
{
	struct ccmd_set_pstate_req *req = buf;
	struct ccmd_set_pstate_rsp *rsp;
	union drm_amdgpu_ctx args = {};

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->pad || (req->op != AMDGPU_CTX_OP_GET_STABLE_PSTATE &&
			 req->op != AMDGPU_CTX_OP_SET_STABLE_PSTATE)) {
		rsp->hdr.ret = -EINVAL;
		return -EINVAL;
	}

	args.in.op = req->op;
	args.in.ctx_id = req->ctx_id;
	args.in.flags = req->flags;
	rsp->hdr.ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
					     DRM_IOCTL_AMDGPU_CTX, &args,
					     sizeof(args));
	if (!rsp->hdr.ret)
		rsp->out_flags = args.out.pstate.flags;
	return 0;
}

/*
 * The query only polls the fence, whatever timeout it names. Every context
 * waits behind the worker, and one SUBMIT_3D could hold thousands of waiting
 * queries. Mesa sends the query only when it debugs a GPU hang.
 */
static int vduse_guest_gpu_cs_query_fence_status(struct vduse_guest_gpu *gpu,
						 struct vduse_guest_gpu_ctx *ctx,
						 void *buf,
						 bool device_timeline)
{
	struct ccmd_cs_query_fence_status_req *req = buf;
	struct ccmd_cs_query_fence_status_rsp *rsp;
	union drm_amdgpu_wait_cs args = {};

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (req->ip_type >= AMDGPU_HW_IP_NUM ||
	    req->ring >= VDUSE_GUEST_GPU_MAX_RINGS) {
		rsp->hdr.ret = -EINVAL;
		return 0;
	}
	if (!req->fence) {
		rsp->expired = 1;
		return 0;
	}

	args.in.handle = req->fence;
	args.in.timeout = 0;
	args.in.ip_type = req->ip_type;
	args.in.ip_instance = req->ip_instance;
	args.in.ring = req->ring;
	args.in.ctx_id = req->ctx_id;
	rsp->hdr.ret = vduse_guest_gpu_ioctl(gpu, ctx->file,
					     DRM_IOCTL_AMDGPU_WAIT_CS, &args,
					     sizeof(args));
	if (!rsp->hdr.ret && !args.out.status)
		rsp->expired = 1;
	return 0;
}

/*
 * Check that count entries of size bytes, aligned to align, fit in a chunk
 * of len bytes at offset.
 */
static bool vduse_guest_gpu_chunk_valid(size_t offset, size_t len,
					size_t count, size_t size,
					size_t align)
{
	return IS_ALIGNED(offset, align) && size_mul(count, size) <= len;
}

/*
 * Translate a CS_SUBMIT's chunks into the worker's memory. The BO list
 * names resources, which become the context's GEM handles. The submission
 * waits for the last one on its timeline and replaces it, through the
 * timeline's syncobj, which it names as both its input and its output.
 */
static int vduse_guest_gpu_cs_submit(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx,
				     void *buf, bool device_timeline)
{
	struct ccmd_cs_submit_req *req = buf;
	struct drm_amdgpu_bo_list_entry *bo_list = NULL;
	struct drm_amdgpu_cs_chunk *chunks = NULL;
	struct drm_amdgpu_bo_list_in list_in;
	struct vduse_guest_gpu_timeline *tl;
	struct drm_amdgpu_cs_chunk_sem sem;
	const struct ccmd_cs_desc *descs;
	union drm_amdgpu_cs cs = {};
	struct drm_syncobj *syncobj;
	void __user *udata = NULL;
	unsigned int i, n = 0;
	size_t descs_len;
	u64 __user *uptrs;
	struct ccmd_rsp *rsp;
	int r;

	rsp = vduse_guest_gpu_rsp(ctx, &req->hdr, sizeof(*rsp));
	if (!rsp)
		return -ENOMEM;
	if (device_timeline || req->num_chunks > VDUSE_GUEST_GPU_MAX_CHUNKS ||
	    !req->ring_idx || req->ring_idx > ctx->nr_timelines) {
		rsp->ret = -EINVAL;
		return -EINVAL;
	}
	tl = &ctx->timelines[req->ring_idx - 1];

	descs_len = size_add(offsetof(struct ccmd_cs_submit_req, payload),
			     size_mul(req->num_chunks, sizeof(*descs)));
	r = -EINVAL;
	if (descs_len > req->hdr.len)
		goto out;
	descs = (const void *)req->payload;

	chunks = kcalloc(req->num_chunks + 2, sizeof(*chunks), GFP_KERNEL);
	r = -ENOMEM;
	if (!chunks)
		goto out;

	for (i = 0; i < req->num_chunks; i++) {
		size_t offset = size_add(descs_len, descs[i].offset);
		size_t len = descs[i].length_dw * 4;
		const void *input = buf + offset;

		r = -EINVAL;
		if (size_add(offset, len) > req->hdr.len)
			goto out;
		chunks[n].chunk_id = descs[i].chunk_id;

		switch (descs[i].chunk_id) {
		case AMDGPU_CHUNK_ID_BO_HANDLES: {
			const struct drm_amdgpu_bo_list_entry *in = input;
			u32 j, count = len / sizeof(*in);

			if (!vduse_guest_gpu_chunk_valid(offset, len, count,
							 sizeof(*in),
							 __alignof__(*in)) ||
			    bo_list)
				goto out;
			bo_list = kmalloc_objs(*bo_list, count);
			r = -ENOMEM;
			if (count && !bo_list)
				goto out;
			r = -EINVAL;
			for (j = 0; j < count; j++) {
				bo_list[j].bo_handle =
					vduse_guest_gpu_handle(ctx,
							       in[j].bo_handle);
				bo_list[j].bo_priority = in[j].bo_priority;
				if (!bo_list[j].bo_handle)
					goto out;
			}

			r = -ENOMEM;
			udata = vduse_guest_gpu_ucopy(gpu, bo_list,
						      count * sizeof(*bo_list));
			if (count && !udata)
				goto out;
			list_in = (struct drm_amdgpu_bo_list_in){
				.operation = ~0,
				.list_handle = ~0,
				.bo_number = count,
				.bo_info_size = sizeof(*bo_list),
				.bo_info_ptr = (uintptr_t)udata,
			};
			udata = vduse_guest_gpu_ucopy(gpu, &list_in,
						      sizeof(list_in));
			chunks[n].length_dw = sizeof(list_in) / 4;
			break;
		}
		case AMDGPU_CHUNK_ID_FENCE: {
			const struct drm_amdgpu_cs_chunk_fence *in = input;
			struct drm_amdgpu_cs_chunk_fence fence;

			if (!vduse_guest_gpu_chunk_valid(offset, len, 1,
							 sizeof(*in),
							 __alignof__(*in)) ||
			    in->offset % sizeof(u64))
				goto out;
			fence.handle = vduse_guest_gpu_handle(ctx, in->handle);
			fence.offset = in->offset;
			if (!fence.handle)
				goto out;
			r = -ENOMEM;
			udata = vduse_guest_gpu_ucopy(gpu, &fence, sizeof(fence));
			chunks[n].length_dw = sizeof(fence) / 4;
			break;
		}
		case AMDGPU_CHUNK_ID_IB:
			if (descs[i].length_dw !=
			    sizeof(struct drm_amdgpu_cs_chunk_ib) / 4)
				goto out;
			fallthrough;
		case AMDGPU_CHUNK_ID_DEPENDENCIES:
			r = -ENOMEM;
			udata = vduse_guest_gpu_ucopy(gpu, input, len);
			chunks[n].length_dw = descs[i].length_dw;
			break;
		default:
			goto out;
		}

		if (!udata)
			goto out;
		chunks[n++].chunk_data = (uintptr_t)udata;
	}

	if (!tl->syncobj) {
		struct drm_syncobj_create create = {
			.flags = DRM_SYNCOBJ_CREATE_SIGNALED,
		};

		r = vduse_guest_gpu_ioctl(gpu, ctx->file,
					  DRM_IOCTL_SYNCOBJ_CREATE, &create,
					  sizeof(create));
		if (r)
			goto out;
		tl->syncobj = create.handle;
	}
	sem.handle = tl->syncobj;
	r = -ENOMEM;
	udata = vduse_guest_gpu_ucopy(gpu, &sem, sizeof(sem));
	if (!udata)
		goto out;
	chunks[n++] = (struct drm_amdgpu_cs_chunk){
		.chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_IN,
		.length_dw = sizeof(sem) / 4,
		.chunk_data = (uintptr_t)udata,
	};
	chunks[n++] = (struct drm_amdgpu_cs_chunk){
		.chunk_id = AMDGPU_CHUNK_ID_SYNCOBJ_OUT,
		.length_dw = sizeof(sem) / 4,
		.chunk_data = (uintptr_t)udata,
	};

	udata = vduse_guest_gpu_ucopy(gpu, chunks, n * sizeof(*chunks));
	uptrs = vduse_guest_gpu_ualloc(gpu, n * sizeof(*uptrs));
	if (!udata || !uptrs)
		goto out;
	for (i = 0; i < n; i++) {
		if (put_user((uintptr_t)udata + i * sizeof(*chunks), &uptrs[i])) {
			r = -EFAULT;
			goto out;
		}
	}

	cs.in.ctx_id = req->ctx_id;
	cs.in.num_chunks = n;
	cs.in.chunks = (uintptr_t)uptrs;
	r = vduse_guest_gpu_ioctl(gpu, ctx->file, DRM_IOCTL_AMDGPU_CS, &cs,
				  sizeof(cs));
	if (r)
		goto out;

	syncobj = drm_syncobj_find(vduse_guest_gpu_drm_file(ctx->file),
				   tl->syncobj);
	if (syncobj) {
		dma_fence_put(tl->last);
		tl->last = drm_syncobj_fence_get(syncobj);
		drm_syncobj_put(syncobj);
	}

out:
	if (r)
		vduse_guest_gpu_async_error(ctx);
	kfree(bo_list);
	kfree(chunks);
	/*
	 * A failed submission is answered with its error and the async
	 * error, so that a process that waits for the sequence number sees
	 * it, and later ccmds still run.
	 */
	rsp->ret = r;
	return 0;
}

static const struct {
	int (*handler)(struct vduse_guest_gpu *gpu,
		       struct vduse_guest_gpu_ctx *ctx, void *buf,
		       bool device_timeline);
	size_t size;
} vduse_guest_gpu_ccmds[] = {
	[CCMD_QUERY_INFO] = { vduse_guest_gpu_query_info,
			      sizeof(struct ccmd_query_info_req) },
	[CCMD_GEM_NEW] = { vduse_guest_gpu_gem_new,
			   sizeof(struct ccmd_gem_new_req) },
	[CCMD_BO_VA_OP] = { vduse_guest_gpu_bo_va_op,
			    sizeof(struct ccmd_bo_va_op_req) },
	[CCMD_CS_SUBMIT] = { vduse_guest_gpu_cs_submit,
			     sizeof(struct ccmd_cs_submit_req) },
	[CCMD_SET_METADATA] = { vduse_guest_gpu_set_metadata,
				sizeof(struct ccmd_set_metadata_req) },
	[CCMD_BO_QUERY_INFO] = { vduse_guest_gpu_bo_query_info,
				 sizeof(struct ccmd_bo_query_info_req) },
	[CCMD_CREATE_CTX] = { vduse_guest_gpu_create_ctx,
			      sizeof(struct ccmd_create_ctx_req) },
	[CCMD_RESERVE_VMID] = { vduse_guest_gpu_reserve_vmid,
				sizeof(struct ccmd_reserve_vmid_req) },
	[CCMD_SET_PSTATE] = { vduse_guest_gpu_set_pstate,
			      sizeof(struct ccmd_set_pstate_req) },
	[CCMD_CS_QUERY_FENCE_STATUS] = {
		vduse_guest_gpu_cs_query_fence_status,
		sizeof(struct ccmd_cs_query_fence_status_req) },
};

/*
 * Run one ccmd on a copy that is at least as large as its struct, with the
 * fields that a shorter ccmd lacks zeroed, as virglrenderer does. Once it
 * has run, copy its response into the shared memory buffer, at most as
 * many bytes as the length that Mesa left there, and then report its
 * sequence number.
 */
static int vduse_guest_gpu_ccmd(struct vduse_guest_gpu *gpu,
				struct vduse_guest_gpu_ctx *ctx,
				const struct ccmd_hdr *hdr,
				bool device_timeline)
{
	struct vduse_guest_gpu_shmem *shmem;
	struct ccmd_rsp *dst;
	size_t size;
	void *buf;
	u32 len;
	int err;

	if (hdr->cmd >= ARRAY_SIZE(vduse_guest_gpu_ccmds) ||
	    !vduse_guest_gpu_ccmds[hdr->cmd].handler)
		return -EINVAL;
	if (!ctx->file)
		return -ENODEV;

	size = max_t(size_t, vduse_guest_gpu_ccmds[hdr->cmd].size, hdr->len);
	buf = kvzalloc(size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	memcpy(buf, hdr, hdr->len);

	vduse_guest_gpu_ureset(gpu);
	err = vduse_guest_gpu_ccmds[hdr->cmd].handler(gpu, ctx, buf,
						       device_timeline);
	kvfree(buf);

	if (!err && ctx->shmem) {
		shmem = ctx->shmem->vaddr;
		if (ctx->rsp) {
			dst = (void *)(shmem + 1) + hdr->rsp_off;
			len = min(((struct ccmd_rsp *)ctx->rsp)->len,
				  READ_ONCE(dst->len));
			memcpy(dst, ctx->rsp, len);
			dst->len = len;
		}
		/* Mesa reads the response once it sees the sequence number. */
		smp_wmb();
		WRITE_ONCE(shmem->seqno, hdr->seqno);
	}
	kvfree(ctx->rsp);
	ctx->rsp = NULL;
	return err;
}

/*
 * Run the ccmds of a SUBMIT_3D in order. A ccmd that fails stops the rest,
 * as in virglrenderer. A CS_SUBMIT is refused in a fenced command of the
 * device's timeline, whose fence would not cover its GPU work.
 */
int vduse_guest_gpu_amdgpu_submit(struct vduse_guest_gpu *gpu,
				  struct vduse_guest_gpu_ctx *ctx,
				  const void *buf, size_t size,
				  bool device_timeline)
{
	int err;

	while (size >= sizeof(struct ccmd_hdr)) {
		const struct ccmd_hdr *hdr = buf;

		if (hdr->len > size || hdr->len < sizeof(*hdr) ||
		    !IS_ALIGNED(hdr->len, 8) || !IS_ALIGNED(hdr->rsp_off, 8))
			return -EINVAL;
		err = vduse_guest_gpu_ccmd(gpu, ctx, hdr, device_timeline);
		if (err)
			return err;
		buf += hdr->len;
		size -= hdr->len;
	}
	return size ? -EINVAL : 0;
}
