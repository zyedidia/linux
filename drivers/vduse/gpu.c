// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-guest-gpu: " fmt

#include <drm/amdgpu_drm.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_ioctl.h>
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
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
#include <linux/xarray.h>
#include <uapi/linux/virtio_gpu.h>
#include <uapi/linux/virtio_ids.h>

#define VDUSE_GUEST_GPU_CTRLQ 0
#define VDUSE_GUEST_GPU_CURSORQ 1
#define VDUSE_GUEST_GPU_INLINE_IOVS 4
#define VDUSE_GUEST_GPU_RENDER "/dev/dri/renderD128"

/*
 * The backend's memory holds the arguments of the ioctls that it runs in
 * its first page and their results in its second.
 */
#define VDUSE_GUEST_GPU_UMEM_SIZE (2 * PAGE_SIZE)

/* Virglrenderer's context type for amdgpu in the DRM capset. */
#define VDUSE_GUEST_GPU_CONTEXT_AMDGPU 2

static const u16 vduse_guest_gpu_queue_sizes[] = {
	[VDUSE_GUEST_GPU_CTRLQ] = 1024,
	[VDUSE_GUEST_GPU_CURSORQ] = 16,
};

/* Libdrm's struct amdgpu_gpu_info, in the layout that the capset embeds. */
struct vduse_guest_gpu_amdgpu_info {
	u32 asic_id;
	u32 chip_rev;
	u32 chip_external_rev;
	u32 family_id;
	u64 ids_flags;
	u64 max_engine_clk;
	u64 max_memory_clk;
	u32 num_shader_engines;
	u32 num_shader_arrays_per_engine;
	u32 avail_quad_shader_pipes;
	u32 max_quad_shader_pipes;
	u32 cache_entries_per_quad_pipe;
	u32 num_hw_gfx_contexts;
	u32 rb_pipes;
	u32 enabled_rb_pipes_mask;
	u32 gpu_counter_freq;
	u32 backend_disable[4];
	u32 mc_arb_ramcfg;
	u32 gb_addr_cfg;
	u32 gb_tile_mode[32];
	u32 gb_macro_tile_mode[16];
	u32 pa_sc_raster_cfg[4];
	u32 pa_sc_raster_cfg1[4];
	u32 cu_active_number;
	u32 cu_ao_mask;
	u32 cu_bitmap[4][4];
	u32 vram_type;
	u32 vram_bit_width;
	u32 ce_ram_size;
	u32 vce_harvest_config;
	u32 pci_rev_id;
};

/*
 * Virglrenderer's struct virgl_renderer_capset_drm for amdgpu, whose fields
 * are in the host's byte order. The union in virglrenderer's definition is
 * as large as its amdgpu member.
 */
struct vduse_guest_gpu_capset {
	u32 wire_format_version;
	u32 version_major;
	u32 version_minor;
	u32 version_patchlevel;
	u32 context_type;
	u32 pad;
	struct {
		u32 address32_hi;
		u32 has_vm_always_valid;
		u64 size_local;
		u64 size_remote;
		struct vduse_guest_gpu_amdgpu_info gpu_info;
		char marketing_name[128];
	} amdgpu;
};
static_assert(sizeof(struct vduse_guest_gpu_capset) == 592);

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
 * Invalid_commands counts the commands answered with an error, and
 * contexts the contexts that exist.
 */
struct vduse_guest_gpu_stats {
	atomic64_t commands;
	atomic64_t invalid_commands;
	atomic_t contexts;
};

struct vduse_guest_gpu_ctx {
	u32 id;
};

struct vduse_guest_gpu {
	struct vduse_guest_dev *dev;
	struct virtio_gpu_config config;
	char *render_path;

	/*
	 * Commands run in queue order on one worker. Running, under lock,
	 * tells the works whether they may use the queues.
	 */
	struct kthread_worker *worker;
	struct kthread_work ctrl_work;
	struct kthread_work cursor_work;
	spinlock_t lock;
	bool running;

	/* The worker's own state, which the works use without a lock. */
	struct xarray contexts;
	struct vduse_guest_gpu_capset capset;
	bool capset_valid;
	struct mm_struct *mm;
	void __user *umem;

	struct vduse_guest_gpu_stats stats;
};

/* The commands that the backend reads into memory, without their data. */
union vduse_guest_gpu_req {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_get_capset_info get_capset_info;
	struct virtio_gpu_get_capset get_capset;
	struct virtio_gpu_ctx_create ctx_create;
	struct virtio_gpu_ctx_destroy ctx_destroy;
	struct virtio_gpu_ctx_resource ctx_resource;
};

/*
 * Give the worker an mm of its own, with memory for the arguments of the
 * ioctls that it runs. The ioctls read and write their arguments with the
 * user copy functions, which reach the mm once the worker adopts it.
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

/* Run an AMDGPU_INFO query. The worker must have adopted its mm. */
static int vduse_guest_gpu_amdgpu_query(struct vduse_guest_gpu *gpu,
					struct file *file,
					struct drm_amdgpu_info *req, void *out,
					u32 size)
{
	void __user *uout = gpu->umem + PAGE_SIZE;
	long err;

	if (size > PAGE_SIZE)
		return -EINVAL;

	req->return_pointer = (uintptr_t)uout;
	req->return_size = size;
	if (copy_to_user(gpu->umem, req, sizeof(*req)))
		return -EFAULT;

	err = file->f_op->unlocked_ioctl(file, DRM_IOCTL_AMDGPU_INFO,
					 (unsigned long)gpu->umem);
	if (err)
		return err;

	if (copy_from_user(out, uout, size))
		return -EFAULT;
	return 0;
}

static int vduse_guest_gpu_amdgpu_regs(struct vduse_guest_gpu *gpu,
				       struct file *file, u32 offset, u32 count,
				       u32 instance, u32 *values)
{
	struct drm_amdgpu_info req = {
		.query = AMDGPU_INFO_READ_MMR_REG,
		.read_mmr_reg = {
			.dword_offset = offset,
			.count = count,
			.instance = instance,
		},
	};

	return vduse_guest_gpu_amdgpu_query(gpu, file, &req, values,
					    count * sizeof(u32));
}

/*
 * Fill the register values of the capset's GPU information, with the
 * queries that libdrm's amdgpu_query_gpu_info_init() makes.
 */
static int vduse_guest_gpu_amdgpu_info_regs(struct vduse_guest_gpu *gpu,
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

			err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0x263d, 1,
							  instance,
							  &info->backend_disable[i]);
			if (err)
				return err;
			info->backend_disable[i] =
				(info->backend_disable[i] >> 16) & 0xff;

			err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0xa0d4, 1,
							  instance,
							  &info->pa_sc_raster_cfg[i]);
			if (err)
				return err;

			if (info->family_id >= AMDGPU_FAMILY_CI) {
				err = vduse_guest_gpu_amdgpu_regs(gpu, file,
								  0xa0d5, 1, instance,
								  &info->pa_sc_raster_cfg1[i]);
				if (err)
					return err;
			}
		}
	}

	err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0x263e, 1, 0xffffffff,
					  &info->gb_addr_cfg);
	if (err)
		return err;

	if (info->family_id < AMDGPU_FAMILY_AI) {
		err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0x2644, 32,
						  0xffffffff,
						  info->gb_tile_mode);
		if (err)
			return err;

		if (info->family_id >= AMDGPU_FAMILY_CI) {
			err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0x2664, 16,
							  0xffffffff,
							  info->gb_macro_tile_mode);
			if (err)
				return err;
		}

		err = vduse_guest_gpu_amdgpu_regs(gpu, file, 0x9d8, 1,
						  0xffffffff,
						  &info->mc_arb_ramcfg);
		if (err)
			return err;
	}

	return 0;
}

/*
 * Fill the amdgpu part of the capset as virglrenderer does through libdrm.
 * The worker must have adopted its mm.
 */
static int vduse_guest_gpu_amdgpu_caps(struct vduse_guest_gpu *gpu,
				       struct file *file,
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
	err = vduse_guest_gpu_amdgpu_query(gpu, file, &req, &accel_working,
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
	err = vduse_guest_gpu_amdgpu_query(gpu, file, &req, di, sizeof(*di));
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

	err = vduse_guest_gpu_amdgpu_info_regs(gpu, file, info);
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
static int vduse_guest_gpu_query_capset(struct vduse_guest_gpu *gpu,
					struct vduse_guest_gpu_capset *caps)
{
	const struct drm_driver *driver;
	struct drm_file *file_priv;
	struct file *file;
	int err;

	err = vduse_guest_gpu_init_mm(gpu);
	if (err)
		return err;

	file = filp_open(gpu->render_path, O_RDWR | O_CLOEXEC, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);

	err = -ENODEV;
	if (!S_ISCHR(file_inode(file)->i_mode) ||
	    imajor(file_inode(file)) != DRM_MAJOR)
		goto out;
	file_priv = file->private_data;
	if (!drm_is_render_client(file_priv))
		goto out;
	driver = file_priv->minor->dev->driver;
	if (strcmp(driver->name, "amdgpu") || driver->major != 3)
		goto out;

	memset(caps, 0, sizeof(*caps));
	caps->version_major = driver->major;
	caps->version_minor = driver->minor;
	caps->version_patchlevel = driver->patchlevel;
	caps->context_type = VDUSE_GUEST_GPU_CONTEXT_AMDGPU;

	kthread_use_mm(gpu->mm);
	err = vduse_guest_gpu_amdgpu_caps(gpu, file, caps);
	kthread_unuse_mm(gpu->mm);

out:
	fput(file);
	return err;
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

	err = vduse_guest_gpu_query_capset(gpu, &gpu->capset);
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
	if (!id)
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

	ctx = kzalloc_obj(*ctx);
	if (!ctx)
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	ctx->id = id;

	err = xa_insert(&gpu->contexts, id, ctx, GFP_KERNEL);
	if (err) {
		kfree(ctx);
		return err == -EBUSY ? VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID :
				       VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	}
	atomic_inc(&gpu->stats.contexts);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

static void vduse_guest_gpu_ctx_free(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx)
{
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
 * The device has no resources, so attaching fails and detaching does
 * nothing, as it does for a resource that is not attached.
 */
static u32 vduse_guest_gpu_ctx_resource(struct vduse_guest_gpu *gpu,
					const struct virtio_gpu_ctx_resource *cmd)
{
	if (!xa_load(&gpu->contexts, le32_to_cpu(cmd->hdr.ctx_id)))
		return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;
	if (le32_to_cpu(cmd->hdr.type) == VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Run a command of in_len bytes, whose start is in req, and return the type
 * of its response. A response with data points out at it.
 */
static u32 vduse_guest_gpu_dispatch(struct vduse_guest_gpu *gpu,
				    const union vduse_guest_gpu_req *req,
				    size_t in_len, const void **out,
				    size_t *out_len)
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
	}

	return VIRTIO_GPU_RESP_ERR_UNSPEC;
}

/*
 * Handle a command chain and return the length written into its response.
 * The response to a fenced command carries its fence, also when it is an
 * error, since the host's driver completes fences from the response.
 */
static u32 vduse_guest_gpu_handle(struct vduse_guest_gpu *gpu,
				  struct vringh_kiov *riov,
				  struct vringh_kiov *wiov)
{
	size_t in_len = vringh_kiov_length(riov);
	size_t room = vringh_kiov_length(wiov);
	struct virtio_gpu_ctrl_hdr resp = {};
	union vduse_guest_gpu_req req = {};
	const void *out = NULL;
	size_t n, out_len = 0;
	struct iov_iter it;
	u32 type;

	atomic64_inc(&gpu->stats.commands);

	n = min(in_len, sizeof(req));
	iov_iter_kvec(&it, ITER_SOURCE, riov->iov, riov->used, in_len);
	if (n < sizeof(req.hdr) || copy_from_iter(&req, n, &it) != n) {
		type = VIRTIO_GPU_RESP_ERR_UNSPEC;
	} else {
		type = vduse_guest_gpu_dispatch(gpu, &req, in_len, &out,
						&out_len);
		if (le32_to_cpu(req.hdr.flags) & VIRTIO_GPU_FLAG_FENCE) {
			resp.flags = cpu_to_le32(VIRTIO_GPU_FLAG_FENCE);
			resp.fence_id = req.hdr.fence_id;
			resp.ctx_id = req.hdr.ctx_id;
			if (le32_to_cpu(req.hdr.flags) &
			    VIRTIO_GPU_FLAG_INFO_RING_IDX) {
				resp.flags |= cpu_to_le32(VIRTIO_GPU_FLAG_INFO_RING_IDX);
				resp.ring_idx = req.hdr.ring_idx;
			}
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

static bool vduse_guest_gpu_running(struct vduse_guest_gpu *gpu)
{
	bool running;

	spin_lock_irq(&gpu->lock);
	running = gpu->running;
	spin_unlock_irq(&gpu->lock);
	return running;
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

		len = 0;
		if (index == VDUSE_GUEST_GPU_CTRLQ)
			len = vduse_guest_gpu_handle(gpu, &riov, &wiov);
		vduse_guest_complete(gpu->dev, index, head, len);
		cond_resched();
	}

	vringh_kiov_cleanup(&riov);
	vringh_kiov_cleanup(&wiov);
}

static void vduse_guest_gpu_ctrl_work(struct kthread_work *work)
{
	struct vduse_guest_gpu *gpu =
		container_of(work, struct vduse_guest_gpu, ctrl_work);

	vduse_guest_gpu_serve(gpu, VDUSE_GUEST_GPU_CTRLQ);
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

/* A reset destroys every context. */
static void vduse_guest_gpu_stop(void *priv)
{
	struct vduse_guest_gpu *gpu = priv;
	struct vduse_guest_gpu_ctx *ctx;
	unsigned long id;

	spin_lock_irq(&gpu->lock);
	gpu->running = false;
	spin_unlock_irq(&gpu->lock);
	kthread_flush_worker(gpu->worker);

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
	struct vduse_guest_gpu *gpu;
	char *opt;
	int err;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "render="))
			render = opt + strlen("render=");
		else if (*opt)
			return ERR_PTR(-EINVAL);
	}

	gpu = kzalloc_obj(*gpu);
	if (!gpu)
		return ERR_PTR(-ENOMEM);
	gpu->dev = dev;
	spin_lock_init(&gpu->lock);
	xa_init(&gpu->contexts);
	kthread_init_work(&gpu->ctrl_work, vduse_guest_gpu_ctrl_work);
	kthread_init_work(&gpu->cursor_work, vduse_guest_gpu_cursor_work);

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

	/*
	 * The device has no scanouts, which the host's driver accepts as a
	 * device without KMS.
	 */
	gpu->config.num_capsets = cpu_to_le32(1);

	/*
	 * The host's driver accepts execbuffers, HOST3D blobs and context
	 * initialization only from a device that offers VIRGL, although
	 * native context does not run virgl.
	 */
	cfg->device_id = VIRTIO_ID_GPU;
	cfg->features = BIT_ULL(VIRTIO_GPU_F_VIRGL) |
			BIT_ULL(VIRTIO_GPU_F_RESOURCE_BLOB) |
			BIT_ULL(VIRTIO_GPU_F_CONTEXT_INIT);
	cfg->num_queues = ARRAY_SIZE(vduse_guest_gpu_queue_sizes);
	cfg->queue_sizes = vduse_guest_gpu_queue_sizes;
	/* A new guest has none of the contexts of the one before it. */
	cfg->allow_reconnect = false;
	cfg->config = &gpu->config;
	cfg->config_size = sizeof(gpu->config);

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

	if (gpu->mm)
		mmput(gpu->mm);
	kthread_destroy_worker(gpu->worker);
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
}

const struct vduse_guest_backend_ops vduse_guest_gpu_ops = {
	.type = "gpu",
	.setup = vduse_guest_gpu_setup,
	.release = vduse_guest_gpu_release,
	.start = vduse_guest_gpu_start,
	.stop = vduse_guest_gpu_stop,
	.kick = vduse_guest_gpu_kick,
	.show = vduse_guest_gpu_show,
};
