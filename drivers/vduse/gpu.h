/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _VDUSE_GUEST_GPU_H
#define _VDUSE_GUEST_GPU_H

#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/maple_tree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/vduse_guest.h>
#include <linux/xarray.h>
#include <uapi/linux/virtio_gpu.h>

struct dma_fence;
struct drm_gem_object;
struct file;
struct mm_struct;

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

/* Libdrm's struct amdgpu_heap_info. */
struct vduse_guest_gpu_heap_info {
	u64 heap_size;
	u64 heap_usage;
	u64 max_allocation;
};

/*
 * Virglrenderer's struct amdvgpu_shmem, at the start of a context's shared
 * memory buffer, whose responses follow at rsp_mem_offset.
 */
struct vduse_guest_gpu_shmem {
	u32 seqno;
	u32 rsp_mem_offset;
	u32 async_error;
	u32 pad;
	struct vduse_guest_gpu_heap_info gtt;
	struct vduse_guest_gpu_heap_info vram;
	struct vduse_guest_gpu_heap_info vis_vram;
};
static_assert(sizeof(struct vduse_guest_gpu_shmem) == 88);

/*
 * A blob resource, which belongs to the device rather than to the context
 * that created it. Blob 0, a context's shared memory buffer, owns its pages
 * and maps them at vaddr. Any other blob holds a reference to its BO. A blob
 * is placed in the host-visible region at map_offset while mapped is set.
 * Blob 0's pages stay mapped meanwhile. A BO's pages are mapped while
 * present is set, which a move of the BO clears and a fault request sets
 * again, and the BO's reservation protects mapped and present. If an unmap
 * fails, the host may still map the pages, so leaked keeps them, and the
 * BO's reference, for the device's lifetime. A leaked BO is pinned, unless
 * the unmap failed in a move, which frees or replaces its pages anyway.
 */
struct vduse_guest_gpu_res {
	struct vduse_guest_gpu *gpu;
	u32 id;
	u32 blob_flags;
	u64 size;
	struct page **pages;
	unsigned long nr_pages;
	void *vaddr;
	struct drm_gem_object *obj;
	u32 bo_domains;
	u64 bo_flags;
	bool mapped;
	bool present;
	bool leaked;
	u64 map_offset;
};

/* A ring's timeline, with the fence of its last command submission. */
struct vduse_guest_gpu_timeline {
	u32 syncobj;
	struct dma_fence *last;
};

/*
 * A context. File is its DRM file on the render node, or NULL if the context
 * could not open one, as in a guest without a GPU. Handles maps the ids of
 * the resources attached to the context to their GEM handles in file, and
 * gem_reqs maps the blob ids of GEM_NEW requests to the requests until their
 * blobs are created. The response of the ccmd that runs is in rsp.
 */
struct vduse_guest_gpu_ctx {
	u32 id;
	struct vduse_guest_gpu_res *shmem;
	struct file *file;
	struct xarray handles;
	struct xarray gem_reqs;
	unsigned int nr_gem_reqs;
	unsigned int nr_timelines;
	struct vduse_guest_gpu_timeline *timelines;
	void *rsp;
};

/*
 * The number of commands answered with an error is in invalid_commands, and
 * the numbers of contexts and resources that exist are in contexts and
 * resources. The number of times that a mapped BO moved and was unmapped is
 * in moves, and the numbers of fault requests answered with a mapping and
 * refused are in faults and faults_refused. The number of fenced flushes
 * that wait for a flip is in flips. Of flips_timed flips, flip_commit_us is
 * the total time of their commits, and flip_wait_us the total time from
 * SET_SCANOUT_BLOB to the flip.
 */
struct vduse_guest_gpu_stats {
	atomic64_t commands;
	atomic64_t invalid_commands;
	atomic_t contexts;
	atomic_t resources;
	atomic64_t moves;
	atomic64_t faults;
	atomic64_t faults_refused;
	atomic64_t flips;
	atomic64_t flip_commit_us;
	atomic64_t flip_wait_us;
	atomic64_t flips_timed;
};

struct vduse_guest_gpu {
	struct vduse_guest_dev *dev;
	struct virtio_gpu_config config;
	struct vduse_guest_shm region;
	char *render_path;
	/*
	 * With vram, which the vram and display options set, a mappable BO
	 * for which Mesa asks VRAM is placed in CPU-visible VRAM. The host
	 * maps such BOs through BAR0, which the command worker registers as
	 * bar_file before it maps the first of them.
	 */
	bool vram;
	u32 bar_file;
	/*
	 * The scanouts on the guest's KMS, with the display option, or NULL,
	 * and the responses of GET_DISPLAY_INFO and GET_EDID.
	 */
	struct vduse_guest_gpu_display *display;
	struct virtio_gpu_resp_display_info display_info;
	struct virtio_gpu_resp_edid edid;

	/*
	 * Commands run in queue order on one worker. The running flag, under
	 * lock, tells the works whether they may use the queues.
	 */
	struct kthread_worker *worker;
	struct kthread_work ctrl_work;
	struct kthread_work cursor_work;
	struct kthread_work fence_work;
	spinlock_t lock;
	bool running;

	/*
	 * Fenced commands wait on fences in waiting, and move to signaled
	 * when their fence signals, under fence_lock.
	 */
	spinlock_t fence_lock;
	struct list_head waiting;
	struct list_head signaled;

	/*
	 * Fault requests wait in faults, under fault_lock, for the fault work
	 * on a worker of their own. The fault work finds a BO's resource by
	 * its region offset in fault_index, while it holds fault_sem for
	 * reading, and the worker that runs commands holds fault_sem for
	 * writing when it adds or removes a resource there.
	 */
	struct kthread_worker *fault_worker;
	struct kthread_work fault_work;
	spinlock_t fault_lock;
	struct list_head faults;
	struct rw_semaphore fault_sem;
	struct maple_tree fault_index;

	/*
	 * The worker's own state, which the works use without a lock. The
	 * worker runs ioctls with their arguments in umem, of which umem_used
	 * bytes are taken, in an mm of its own. The shared memory buffers take
	 * shmem_size bytes, and the contexts' GEM_NEW requests number
	 * nr_gem_reqs.
	 */
	struct xarray contexts;
	struct xarray resources;
	struct vduse_guest_gpu_capset capset;
	bool capset_valid;
	struct virtio_gpu_resp_map_info map_info;
	struct mm_struct *mm;
	void __user *umem;
	size_t umem_used;
	size_t shmem_size;
	unsigned int nr_gem_reqs;

	struct vduse_guest_gpu_stats stats;
};

void __user *vduse_guest_gpu_ualloc(struct vduse_guest_gpu *gpu, size_t size);
void vduse_guest_gpu_ureset(struct vduse_guest_gpu *gpu);

int vduse_guest_gpu_amdgpu_capset(struct vduse_guest_gpu *gpu,
				  struct vduse_guest_gpu_capset *caps);
void vduse_guest_gpu_amdgpu_ctx_init(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx);
void vduse_guest_gpu_amdgpu_ctx_fini(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx);
int vduse_guest_gpu_amdgpu_submit(struct vduse_guest_gpu *gpu,
				  struct vduse_guest_gpu_ctx *ctx,
				  const void *buf, size_t size,
				  bool device_timeline);
struct dma_fence *vduse_guest_gpu_amdgpu_fence(struct vduse_guest_gpu_ctx *ctx,
					       u32 ring_idx);
void vduse_guest_gpu_amdgpu_update_heaps(struct vduse_guest_gpu *gpu,
					 struct vduse_guest_gpu_ctx *ctx);
int vduse_guest_gpu_amdgpu_create_bo(struct vduse_guest_gpu *gpu,
				     struct vduse_guest_gpu_ctx *ctx,
				     struct vduse_guest_gpu_res *res,
				     u64 blob_id);
int vduse_guest_gpu_amdgpu_attach(struct vduse_guest_gpu_ctx *ctx,
				  struct vduse_guest_gpu_res *res);
void vduse_guest_gpu_amdgpu_detach(struct vduse_guest_gpu_ctx *ctx,
				   u32 res_id);
int vduse_guest_gpu_amdgpu_pages(struct vduse_guest_gpu_res *res,
				 struct page ***pages);
void vduse_guest_gpu_amdgpu_set_notify(struct vduse_guest_gpu_res *res,
				       void (*fn)(void *priv));
int vduse_guest_gpu_amdgpu_vram(struct vduse_guest_gpu_res *res,
				int (*fn)(void *arg, u64 offset, u64 size),
				void *arg);
int vduse_guest_gpu_amdgpu_reg_bar(struct vduse_guest_gpu_res *res, u32 *file);
int vduse_guest_gpu_amdgpu_pin(struct vduse_guest_gpu_res *res);
struct drm_framebuffer *
vduse_guest_gpu_amdgpu_fb(struct vduse_guest_gpu *gpu,
			  struct vduse_guest_gpu_res *res,
			  const struct drm_mode_fb_cmd2 *cmd);

bool vduse_guest_gpu_running(struct vduse_guest_gpu *gpu);
int vduse_guest_gpu_display_init(struct vduse_guest_gpu *gpu);
void vduse_guest_gpu_display_fini(struct vduse_guest_gpu *gpu);
void vduse_guest_gpu_display_reset(struct vduse_guest_gpu *gpu);
void vduse_guest_gpu_display_clear(struct vduse_guest_gpu *gpu, u32 events);
u32 vduse_guest_gpu_display_info(struct vduse_guest_gpu *gpu,
				 struct virtio_gpu_resp_display_info *info);
u32 vduse_guest_gpu_set_scanout(struct vduse_guest_gpu *gpu,
				const struct virtio_gpu_set_scanout *cmd);
u32 vduse_guest_gpu_set_scanout_blob(struct vduse_guest_gpu *gpu,
				     const struct virtio_gpu_set_scanout_blob *cmd);
struct dma_fence *vduse_guest_gpu_flush_fence(struct vduse_guest_gpu *gpu,
					      u32 res_id);
u32 vduse_guest_gpu_get_edid(struct vduse_guest_gpu *gpu,
			     const struct virtio_gpu_cmd_get_edid *cmd,
			     struct virtio_gpu_resp_edid *resp);

#endif
