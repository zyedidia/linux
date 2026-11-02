// SPDX-License-Identifier: GPL-2.0
/*
 * Scanouts of the virtio-gpu backend on the guest's KMS. Each connector of
 * the DRM device behind the render node is a scanout. SET_SCANOUT_BLOB makes
 * a framebuffer from a blob's BO and commits it, without waiting for the flip
 * unless the commit sets a mode, and the flip signals a fence. A fenced
 * RESOURCE_FLUSH of a blob on a scanout completes at that flip. The commands
 * run on the command worker, and the hotplug callback only raises display
 * events.
 */

#define pr_fmt(fmt) "vduse-guest-gpu: " fmt

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_uapi.h>
#include <drm/drm_client.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_encoder.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_vblank.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/slab.h>
#include <linux/vduse_guest.h>
#include <uapi/linux/virtio_gpu.h>

#include "gpu.h"

/*
 * A scanout shows res_id on crtc, which it takes when it is enabled and keeps
 * until a disable of it succeeds, in mode.
 * Flip is the fence of its latest commit, which signals at the flip, on the
 * scanout's own fence context, and flip_start when its command arrived.
 */
struct vduse_guest_gpu_scanout {
	struct drm_connector *connector;
	struct drm_crtc *crtc;
	struct drm_display_mode mode;
	u32 res_id;
	struct dma_fence *flip;
	ktime_t flip_start;
	u64 fence_context;
	u64 fence_seqno;
};

/*
 * The scanouts of the DRM device of file. The command worker uses them
 * without a lock. The hotplug callback raises VIRTIO_GPU_EVENT_DISPLAY in the
 * configuration and publishes it under events_lock, and a write of
 * events_clear clears it. DRM cannot unregister its client, so the display
 * outlives a backend that is released, with gpu set to NULL. The flip fences,
 * which signal in the vblank interrupt, take fence_lock.
 */
struct vduse_guest_gpu_display {
	struct vduse_guest_gpu *gpu;
	struct file *file;
	struct drm_device *drm;
	struct drm_client_dev client;
	unsigned int nr_scanouts;
	struct vduse_guest_gpu_scanout scanouts[VIRTIO_GPU_MAX_SCANOUTS];
	spinlock_t fence_lock;
	struct mutex events_lock;
};

static const char *vduse_guest_gpu_flip_name(struct dma_fence *fence)
{
	return "vduse-gpu-flip";
}

static const struct dma_fence_ops vduse_guest_gpu_flip_ops = {
	.get_driver_name = vduse_guest_gpu_flip_name,
	.get_timeline_name = vduse_guest_gpu_flip_name,
};

/* Publish events_read to the host, with events_lock held. */
static void vduse_guest_gpu_display_events(struct vduse_guest_gpu *gpu)
{
	__le32 events = gpu->config.events_read;

	vduse_guest_set_config(gpu->dev,
			       offsetof(struct virtio_gpu_config, events_read),
			       &events, sizeof(events));
}

static int vduse_guest_gpu_display_hotplug(struct drm_client_dev *client)
{
	struct vduse_guest_gpu_display *d =
		container_of(client, struct vduse_guest_gpu_display, client);
	struct vduse_guest_gpu *gpu;

	mutex_lock(&d->events_lock);
	gpu = d->gpu;
	if (gpu) {
		gpu->config.events_read |=
			cpu_to_le32(VIRTIO_GPU_EVENT_DISPLAY);
		/* A host driver that starts reads the displays itself. */
		if (vduse_guest_gpu_running(gpu)) {
			vduse_guest_gpu_display_events(gpu);
			vduse_guest_config_irq(gpu->dev);
		}
	}
	mutex_unlock(&d->events_lock);
	return 0;
}

static const struct drm_client_funcs vduse_guest_gpu_client_funcs = {
	.hotplug = vduse_guest_gpu_display_hotplug,
};

/* Clear the display events that the host's driver acknowledges. */
void vduse_guest_gpu_display_clear(struct vduse_guest_gpu *gpu, u32 events)
{
	struct vduse_guest_gpu_display *d = gpu->display;

	if (!d)
		return;
	mutex_lock(&d->events_lock);
	gpu->config.events_read &= ~cpu_to_le32(events);
	vduse_guest_gpu_display_events(gpu);
	mutex_unlock(&d->events_lock);
}

/*
 * Open the render node. Setup runs before userspace mounts devtmpfs, so a
 * node under /dev/ is opened in a mount of devtmpfs of its own, which has
 * the same nodes.
 */
static struct file *vduse_guest_gpu_open_render(const char *path)
{
	struct file_system_type *type;
	struct vfsmount *mnt;
	struct file *file;

	if (!str_has_prefix(path, "/dev/"))
		return filp_open(path, O_RDWR | O_CLOEXEC, 0);
	type = get_fs_type("devtmpfs");
	if (!type)
		return ERR_PTR(-ENODEV);
	mnt = vfs_kern_mount(type, 0, "devtmpfs", NULL);
	put_filesystem(type);
	if (IS_ERR(mnt))
		return ERR_CAST(mnt);
	file = file_open_root_mnt(mnt, path + strlen("/dev/"),
				  O_RDWR | O_CLOEXEC, 0);
	mntput(mnt);
	return file;
}

/*
 * Find the guest's DRM device through the render node, and make a scanout
 * of each of its connectors but writeback ones. The device keeps no
 * scanouts if the guest has no KMS.
 */
int vduse_guest_gpu_display_init(struct vduse_guest_gpu *gpu)
{
	struct drm_connector_list_iter iter;
	struct vduse_guest_gpu_display *d;
	struct drm_connector *connector;
	struct drm_file *file_priv;
	unsigned int i;
	int err;

	d = kzalloc_obj(*d);
	if (!d)
		return -ENOMEM;
	d->gpu = gpu;
	mutex_init(&d->events_lock);
	spin_lock_init(&d->fence_lock);

	d->file = vduse_guest_gpu_open_render(gpu->render_path);
	if (IS_ERR(d->file)) {
		err = PTR_ERR(d->file);
		goto free;
	}
	file_priv = d->file->private_data;
	d->drm = file_priv->minor->dev;
	if (!drm_core_check_feature(d->drm, DRIVER_MODESET) ||
	    !drm_core_check_feature(d->drm, DRIVER_ATOMIC)) {
		err = -ENODEV;
		goto put;
	}

	drm_connector_list_iter_begin(d->drm, &iter);
	drm_for_each_connector_iter(connector, &iter) {
		if (d->nr_scanouts == VIRTIO_GPU_MAX_SCANOUTS)
			break;
		if (connector->connector_type == DRM_MODE_CONNECTOR_WRITEBACK)
			continue;
		drm_connector_get(connector);
		d->scanouts[d->nr_scanouts++].connector = connector;
	}
	drm_connector_list_iter_end(&iter);
	for (i = 0; i < d->nr_scanouts; i++)
		d->scanouts[i].fence_context = dma_fence_context_alloc(1);

	err = drm_client_init(d->drm, &d->client, "vduse-gpu",
			      &vduse_guest_gpu_client_funcs);
	if (err)
		goto connectors;
	/* Registering the client calls its hotplug callback. */
	gpu->display = d;
	gpu->config.num_scanouts = cpu_to_le32(d->nr_scanouts);
	drm_client_register(&d->client);
	return 0;

connectors:
	while (d->nr_scanouts)
		drm_connector_put(d->scanouts[--d->nr_scanouts].connector);
put:
	fput(d->file);
free:
	kfree(d);
	return err;
}

/*
 * The registered client keeps the display, and the hotplug callback finds
 * no backend.
 */
void vduse_guest_gpu_display_fini(struct vduse_guest_gpu *gpu)
{
	struct vduse_guest_gpu_display *d = gpu->display;

	if (!d)
		return;
	mutex_lock(&d->events_lock);
	d->gpu = NULL;
	mutex_unlock(&d->events_lock);
	gpu->display = NULL;
}

/*
 * Return the progressive mode of a connector for a size, the preferred one if
 * it has the size, and otherwise the one with the highest refresh rate. The
 * caller holds mode_config.mutex.
 */
static struct drm_display_mode *
vduse_guest_gpu_mode(struct drm_connector *connector, u32 width, u32 height)
{
	struct drm_display_mode *mode, *best = NULL;

	list_for_each_entry(mode, &connector->modes, head) {
		if (mode->hdisplay != width || mode->vdisplay != height ||
		    (mode->flags & (DRM_MODE_FLAG_INTERLACE |
				    DRM_MODE_FLAG_DBLSCAN)))
			continue;
		if (mode->type & DRM_MODE_TYPE_PREFERRED)
			return mode;
		if (!best || drm_mode_vrefresh(mode) > drm_mode_vrefresh(best))
			best = mode;
	}
	return best;
}

static bool vduse_guest_gpu_dtd(u8 *d, const struct drm_display_mode *m,
				const struct drm_display_info *info);

/*
 * Return the mode that the EDID lists first: the one set for the preferred
 * mode's size, or if its timing does not fit a timing descriptor, the first
 * mode set for its size that fits. The caller holds mode_config.mutex.
 */
static struct drm_display_mode *
vduse_guest_gpu_preferred(struct drm_connector *connector)
{
	struct drm_display_mode *mode, *m;
	u8 dtd[18];

	list_for_each_entry(mode, &connector->modes, head) {
		if (!(mode->type & DRM_MODE_TYPE_PREFERRED))
			continue;
		m = vduse_guest_gpu_mode(connector, mode->hdisplay,
					 mode->vdisplay);
		if (vduse_guest_gpu_dtd(dtd, m, &connector->display_info))
			return m;
		break;
	}
	list_for_each_entry(mode, &connector->modes, head) {
		if (vduse_guest_gpu_mode(connector, mode->hdisplay,
					 mode->vdisplay) == mode &&
		    vduse_guest_gpu_dtd(dtd, mode, &connector->display_info))
			return mode;
	}
	return NULL;
}

/* Probe a connector's modes, with mode_config.mutex held. */
static void vduse_guest_gpu_probe(struct vduse_guest_gpu_display *d,
				  struct drm_connector *c)
{
	struct drm_mode_config *config = &d->drm->mode_config;

	c->funcs->fill_modes(c, config->max_width, config->max_height);
}

/*
 * Probe every connector and report the mode that its EDID lists first for
 * each connected one.
 */
u32 vduse_guest_gpu_display_info(struct vduse_guest_gpu *gpu,
				 struct virtio_gpu_resp_display_info *info)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	struct drm_mode_config *config;
	struct drm_display_mode *mode;
	struct drm_connector *c;
	unsigned int i;

	memset(info, 0, sizeof(*info));
	if (!d)
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	config = &d->drm->mode_config;

	mutex_lock(&config->mutex);
	for (i = 0; i < d->nr_scanouts; i++) {
		c = d->scanouts[i].connector;
		vduse_guest_gpu_probe(d, c);
		mode = vduse_guest_gpu_preferred(c);
		if (c->status != connector_status_connected || !mode)
			continue;
		info->pmodes[i].r.width = cpu_to_le32(mode->hdisplay);
		info->pmodes[i].r.height = cpu_to_le32(mode->vdisplay);
		info->pmodes[i].enabled = cpu_to_le32(1);
	}
	mutex_unlock(&config->mutex);
	return VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
}

/*
 * Take a CRTC that the connector can drive and no other scanout uses. A
 * scanout keeps it until a disable of it succeeds.
 */
static struct drm_crtc *
vduse_guest_gpu_take_crtc(struct vduse_guest_gpu_display *d,
			  struct vduse_guest_gpu_scanout *so)
{
	struct drm_encoder *encoder;
	struct drm_crtc *crtc;
	u32 mask = 0, used = 0;
	unsigned int i;

	if (so->crtc)
		return so->crtc;
	for (i = 0; i < d->nr_scanouts; i++) {
		if (d->scanouts[i].crtc)
			used |= drm_crtc_mask(d->scanouts[i].crtc);
	}
	/* The connector's current CRTC comes first. */
	crtc = so->connector->state ? so->connector->state->crtc : NULL;
	if (crtc && !(used & drm_crtc_mask(crtc))) {
		so->crtc = crtc;
		return crtc;
	}
	drm_connector_for_each_possible_encoder(so->connector, encoder)
		mask |= encoder->possible_crtcs;
	drm_for_each_crtc(crtc, d->drm) {
		if ((mask & ~used) & drm_crtc_mask(crtc)) {
			so->crtc = crtc;
			return crtc;
		}
	}
	return NULL;
}

/*
 * The commit of a scanout. Fb, if set, goes on the CRTC's primary plane from
 * the source rectangle src, in 16.16 fixed point, with mode, and a flip
 * fence signals at the flip. Without fb, the scanout is disabled.
 */
struct vduse_guest_gpu_commit {
	struct vduse_guest_gpu_scanout *so;
	struct drm_framebuffer *fb;
	const struct drm_display_mode *mode;
	struct drm_display_mode mode_buf;
	struct drm_rect src;
	bool modeset;
	struct dma_fence *flip;
	struct drm_pending_vblank_event *event;
};

static int vduse_guest_gpu_build(struct drm_atomic_commit *state,
				 struct vduse_guest_gpu_commit *c)
{
	struct vduse_guest_gpu_scanout *so = c->so;
	struct drm_connector_state *conn_state;
	struct drm_plane_state *plane_state;
	struct drm_crtc_state *crtc_state;
	int err;

	/*
	 * A state that the kernel allocates allows modesets, which amdgpu
	 * treats as a full update of every plane, so a flip disallows them.
	 */
	state->allow_modeset = !c->fb || c->modeset;
	crtc_state = drm_atomic_get_crtc_state(state, so->crtc);
	if (IS_ERR(crtc_state))
		return PTR_ERR(crtc_state);
	plane_state = drm_atomic_get_plane_state(state, so->crtc->primary);
	if (IS_ERR(plane_state))
		return PTR_ERR(plane_state);
	if (state->allow_modeset) {
		conn_state = drm_atomic_get_connector_state(state,
							    so->connector);
		if (IS_ERR(conn_state))
			return PTR_ERR(conn_state);
	}

	if (!c->fb) {
		crtc_state->active = false;
		err = drm_atomic_set_mode_for_crtc(crtc_state, NULL);
		if (err)
			return err;
		err = drm_atomic_set_crtc_for_connector(conn_state, NULL);
		if (err)
			return err;
		err = drm_atomic_set_crtc_for_plane(plane_state, NULL);
		if (err)
			return err;
		drm_atomic_set_fb_for_plane(plane_state, NULL);
		return 0;
	}

	if (c->modeset) {
		err = drm_atomic_set_mode_for_crtc(crtc_state, c->mode);
		if (err)
			return err;
		crtc_state->active = true;
		err = drm_atomic_set_crtc_for_connector(conn_state, so->crtc);
		if (err)
			return err;
	}
	err = drm_atomic_set_crtc_for_plane(plane_state, so->crtc);
	if (err)
		return err;
	drm_atomic_set_fb_for_plane(plane_state, c->fb);
	plane_state->src_x = c->src.x1;
	plane_state->src_y = c->src.y1;
	plane_state->src_w = drm_rect_width(&c->src);
	plane_state->src_h = drm_rect_height(&c->src);
	plane_state->crtc_x = 0;
	plane_state->crtc_y = 0;
	plane_state->crtc_w = c->mode->hdisplay;
	plane_state->crtc_h = c->mode->vdisplay;

	c->event = kzalloc_obj(*c->event);
	if (!c->event)
		return -ENOMEM;
	c->event->event.base.type = DRM_EVENT_FLIP_COMPLETE;
	c->event->event.base.length = sizeof(c->event->event);
	c->event->base.event = &c->event->event.base;
	c->event->base.fence = dma_fence_get(c->flip);
	crtc_state->event = c->event;
	return 0;
}

/*
 * Commit a scanout's change, without waiting for the flip unless it disables
 * the scanout or sets a mode.
 */
static int vduse_guest_gpu_commit(struct vduse_guest_gpu_display *d,
				  struct vduse_guest_gpu_commit *c)
{
	struct drm_modeset_acquire_ctx ctx;
	struct drm_atomic_commit *state;
	struct drm_crtc_state *crtc_state;
	int err;

	state = drm_atomic_commit_alloc(d->drm);
	if (!state)
		return -ENOMEM;
	drm_modeset_acquire_init(&ctx, 0);
	state->acquire_ctx = &ctx;
retry:
	err = vduse_guest_gpu_build(state, c);
	if (!err)
		err = c->fb && !c->modeset ?
			drm_atomic_nonblocking_commit(state) :
			drm_atomic_commit(state);
	if (err && c->event) {
		crtc_state = drm_atomic_get_new_crtc_state(state, c->so->crtc);
		if (crtc_state)
			crtc_state->event = NULL;
		drm_event_cancel_free(d->drm, &c->event->base);
	}
	c->event = NULL;
	if (err == -EDEADLK) {
		drm_atomic_commit_clear(state);
		drm_modeset_backoff(&ctx);
		goto retry;
	}
	drm_atomic_commit_put(state);
	drm_modeset_drop_locks(&ctx);
	drm_modeset_acquire_fini(&ctx);
	return err;
}

/*
 * Count the time from a scanout's latest SET_SCANOUT_BLOB to its flip, once
 * the flip has signaled, unless it was a modeset.
 */
static void vduse_guest_gpu_time_flip(struct vduse_guest_gpu *gpu,
				      struct vduse_guest_gpu_scanout *so)
{
	if (!so->flip || !so->res_id || so->flip->error ||
	    !dma_fence_is_signaled(so->flip) || !so->flip_start)
		return;
	atomic64_add(ktime_us_delta(dma_fence_timestamp(so->flip),
				    so->flip_start), &gpu->stats.flip_wait_us);
	atomic64_inc(&gpu->stats.flips_timed);
}

/* Wait for a scanout's latest flip, which a new commit must not overtake. */
static void vduse_guest_gpu_wait_flip(struct vduse_guest_gpu_scanout *so)
{
	if (so->flip)
		dma_fence_wait_timeout(so->flip, false, msecs_to_jiffies(100));
}

static void vduse_guest_gpu_disable(struct vduse_guest_gpu_display *d,
				    struct vduse_guest_gpu_scanout *so)
{
	struct vduse_guest_gpu_commit c = { .so = so };
	int err;

	/* A scanout holds a CRTC while it is on or a disable of it failed. */
	if (!so->crtc)
		return;
	vduse_guest_gpu_wait_flip(so);
	err = vduse_guest_gpu_commit(d, &c);
	if (err)
		pr_warn("%s: cannot disable scanout %td: %d\n",
			vduse_guest_name(d->gpu->dev), so - d->scanouts, err);
	/* The CRTC still drives the connector if the commit failed. */
	if (!err)
		so->crtc = NULL;
	so->res_id = 0;
	memset(&so->mode, 0, sizeof(so->mode));
}

/* The DRM format of each virtio-gpu format, which names bytes in memory. */
static u32 vduse_guest_gpu_fourcc(u32 format)
{
	switch (format) {
	case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
		return DRM_FORMAT_ARGB8888;
	case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
		return DRM_FORMAT_XRGB8888;
	case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
		return DRM_FORMAT_BGRA8888;
	case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:
		return DRM_FORMAT_BGRX8888;
	case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
		return DRM_FORMAT_ABGR8888;
	case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:
		return DRM_FORMAT_RGBX8888;
	case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
		return DRM_FORMAT_RGBA8888;
	case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
		return DRM_FORMAT_XBGR8888;
	}
	return 0;
}

/*
 * Show a blob on a scanout, in the connector's mode for the rectangle's size.
 * A new mode or a scanout that was off needs a modeset, which the command
 * waits for, and otherwise the command returns before the flip.
 */
u32 vduse_guest_gpu_set_scanout_blob(struct vduse_guest_gpu *gpu,
				     const struct virtio_gpu_set_scanout_blob *cmd)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	u32 id = le32_to_cpu(cmd->scanout_id);
	u32 res_id = le32_to_cpu(cmd->resource_id);
	u32 x = le32_to_cpu(cmd->r.x), y = le32_to_cpu(cmd->r.y);
	u32 w = le32_to_cpu(cmd->r.width), h = le32_to_cpu(cmd->r.height);
	struct vduse_guest_gpu_commit c = {};
	struct drm_mode_fb_cmd2 fb_cmd = {};
	struct vduse_guest_gpu_scanout *so;
	struct drm_display_mode *mode;
	struct vduse_guest_gpu_res *res;
	ktime_t start = ktime_get(), commit;
	bool took;
	int err;

	if (!d || id >= d->nr_scanouts)
		return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
	so = &d->scanouts[id];
	if (!res_id) {
		vduse_guest_gpu_disable(d, so);
		return VIRTIO_GPU_RESP_OK_NODATA;
	}
	res = xa_load(&gpu->resources, res_id);
	if (!res || !res->obj)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	fb_cmd.width = le32_to_cpu(cmd->width);
	fb_cmd.height = le32_to_cpu(cmd->height);
	fb_cmd.pixel_format = vduse_guest_gpu_fourcc(le32_to_cpu(cmd->format));
	fb_cmd.pitches[0] = le32_to_cpu(cmd->strides[0]);
	fb_cmd.offsets[0] = le32_to_cpu(cmd->offsets[0]);
	if (!fb_cmd.pixel_format || !w || !h || x > fb_cmd.width ||
	    w > fb_cmd.width - x || y > fb_cmd.height ||
	    h > fb_cmd.height - y)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	mutex_lock(&d->drm->mode_config.mutex);
	mode = vduse_guest_gpu_mode(so->connector, w, h);
	if (mode)
		drm_mode_copy(&c.mode_buf, mode);
	mutex_unlock(&d->drm->mode_config.mutex);
	took = !so->crtc;
	if (!mode || !vduse_guest_gpu_take_crtc(d, so))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	c.fb = vduse_guest_gpu_amdgpu_fb(gpu, res, &fb_cmd);
	if (IS_ERR(c.fb)) {
		err = PTR_ERR(c.fb);
		goto fail;
	}

	c.so = so;
	c.mode = &c.mode_buf;
	c.modeset = !so->res_id || !drm_mode_equal(&so->mode, c.mode);
	drm_rect_init(&c.src, x << 16, y << 16, w << 16, h << 16);
	c.flip = kzalloc_obj(*c.flip);
	if (!c.flip) {
		drm_framebuffer_put(c.fb);
		err = -ENOMEM;
		goto fail;
	}

	/*
	 * Wait for the previous flip before the new fence takes its seqno, so
	 * that a fence that fails signals after it.
	 */
	vduse_guest_gpu_wait_flip(so);
	vduse_guest_gpu_time_flip(gpu, so);
	dma_fence_init(c.flip, &vduse_guest_gpu_flip_ops, &d->fence_lock,
		       so->fence_context, ++so->fence_seqno);
	commit = ktime_get();
	err = vduse_guest_gpu_commit(d, &c);
	commit = ktime_sub(ktime_get(), commit);
	drm_framebuffer_put(c.fb);
	if (err) {
		dma_fence_set_error(c.flip, err);
		dma_fence_signal(c.flip);
		dma_fence_put(c.flip);
		pr_warn_ratelimited("%s: cannot show resource %u on scanout %u: %d\n",
				    vduse_guest_name(gpu->dev), res_id, id, err);
		goto fail;
	}

	dma_fence_put(so->flip);
	so->flip = c.flip;
	so->flip_start = c.modeset ? 0 : start;
	if (!c.modeset)
		atomic64_add(ktime_to_us(commit), &gpu->stats.flip_commit_us);
	so->res_id = res_id;
	drm_mode_copy(&so->mode, c.mode);
	return VIRTIO_GPU_RESP_OK_NODATA;

fail:
	/* A CRTC that this command took is given back. */
	if (took)
		so->crtc = NULL;
	return err == -ENOMEM ? VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY :
				VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
}

/*
 * SET_SCANOUT disables a scanout. SET_SCANOUT with a 2D resource is refused.
 */
u32 vduse_guest_gpu_set_scanout(struct vduse_guest_gpu *gpu,
				const struct virtio_gpu_set_scanout *cmd)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	u32 id = le32_to_cpu(cmd->scanout_id);

	if (!d || id >= d->nr_scanouts)
		return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
	if (cmd->resource_id)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
	vduse_guest_gpu_disable(d, &d->scanouts[id]);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

/*
 * Return the fence of the latest flip of a scanout that shows the resource,
 * which a fenced flush waits for, or NULL.
 */
struct dma_fence *vduse_guest_gpu_flush_fence(struct vduse_guest_gpu *gpu,
					      u32 res_id)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	unsigned int i;

	if (!d || !res_id)
		return NULL;
	for (i = 0; i < d->nr_scanouts; i++) {
		if (d->scanouts[i].res_id == res_id && d->scanouts[i].flip) {
			if (!dma_fence_is_signaled(d->scanouts[i].flip))
				atomic64_inc(&gpu->stats.flips);
			return dma_fence_get(d->scanouts[i].flip);
		}
	}
	return NULL;
}

/* A reset turns every scanout off. */
void vduse_guest_gpu_display_reset(struct vduse_guest_gpu *gpu)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	unsigned int i;

	if (!d)
		return;
	for (i = 0; i < d->nr_scanouts; i++) {
		vduse_guest_gpu_disable(d, &d->scanouts[i]);
		dma_fence_put(d->scanouts[i].flip);
		d->scanouts[i].flip = NULL;
	}
}

#define VDUSE_GUEST_GPU_EDID_BLOCK 128
#define VDUSE_GUEST_GPU_EDID_DTDS 4

/* Encode a mode as an 18-byte detailed timing descriptor. */
static bool vduse_guest_gpu_dtd(u8 *d, const struct drm_display_mode *m,
				const struct drm_display_info *info)
{
	u32 hblank = m->htotal - m->hdisplay, vblank = m->vtotal - m->vdisplay;
	u32 hfp = m->hsync_start - m->hdisplay;
	u32 hsw = m->hsync_end - m->hsync_start;
	u32 vfp = m->vsync_start - m->vdisplay;
	u32 vsw = m->vsync_end - m->vsync_start;
	u32 clock = m->clock / 10;
	u32 hmm = info->width_mm, vmm = info->height_mm;

	/*
	 * The host's EDID parser drops small modes and modes without sync, and
	 * the backend sets neither interlaced nor double-scan modes.
	 */
	if (!clock || clock > 0xffff || m->hdisplay > 0xfff ||
	    hblank > 0xfff || m->vdisplay > 0xfff || vblank > 0xfff ||
	    hfp > 0x3ff || hsw > 0x3ff || vfp > 0x3f || vsw > 0x3f ||
	    m->hdisplay < 64 || m->vdisplay < 64 || !hsw || !vsw ||
	    (m->flags & (DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLSCAN)))
		return false;
	if (hmm > 0xfff || vmm > 0xfff)
		hmm = vmm = 0;

	d[0] = clock & 0xff;
	d[1] = clock >> 8;
	d[2] = m->hdisplay & 0xff;
	d[3] = hblank & 0xff;
	d[4] = (m->hdisplay >> 8) << 4 | hblank >> 8;
	d[5] = m->vdisplay & 0xff;
	d[6] = vblank & 0xff;
	d[7] = (m->vdisplay >> 8) << 4 | vblank >> 8;
	d[8] = hfp & 0xff;
	d[9] = hsw & 0xff;
	d[10] = (vfp & 0xf) << 4 | (vsw & 0xf);
	d[11] = (hfp >> 8) << 6 | (hsw >> 8) << 4 | (vfp >> 4) << 2 | vsw >> 4;
	d[12] = hmm & 0xff;
	d[13] = vmm & 0xff;
	d[14] = (hmm >> 8) << 4 | vmm >> 8;
	/* Digital separate sync, with the mode's polarities. */
	d[17] = 0x18;
	if (m->flags & DRM_MODE_FLAG_PVSYNC)
		d[17] |= 0x04;
	if (m->flags & DRM_MODE_FLAG_PHSYNC)
		d[17] |= 0x02;
	return true;
}

static void vduse_guest_gpu_edid_checksum(u8 *block)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < VDUSE_GUEST_GPU_EDID_BLOCK - 1; i++)
		sum += block[i];
	block[VDUSE_GUEST_GPU_EDID_BLOCK - 1] = -sum;
}

/*
 * Return an EDID of a connector that lists one mode for each size, the one
 * that SET_SCANOUT_BLOB sets for it, so that the host can only choose modes
 * that the backend sets. It is a base block alone, with the mode that
 * vduse_guest_gpu_preferred() returns and up to three more, since a CTA-861
 * extension makes the host add modes of its own. The connector's physical
 * size goes in it, under a vendor of its own. The caller holds
 * mode_config.mutex.
 */
static size_t vduse_guest_gpu_edid(struct drm_connector *c, u8 *edid)
{
	static const u8 header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	/* Manufacturer IVT, product 0, serial 0, week 1 of 2026. */
	static const u8 vendor[10] = { 0x26, 0xd4, 0, 0, 0, 0, 0, 0, 1, 36 };
	static const u8 srgb[10] = {
		0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54,
	};
	struct drm_display_mode *mode, *m;
	unsigned int n, i;

	mode = vduse_guest_gpu_preferred(c);
	if (!mode)
		return 0;

	memset(edid, 0, VDUSE_GUEST_GPU_EDID_BLOCK);
	memcpy(edid, header, sizeof(header));
	memcpy(edid + 8, vendor, sizeof(vendor));
	/* EDID 1.4 with a digital input, its size in cm and gamma 2.2. */
	edid[18] = 1;
	edid[19] = 4;
	edid[20] = 0x80;
	edid[21] = min(c->display_info.width_mm / 10, 255U);
	edid[22] = min(c->display_info.height_mm / 10, 255U);
	edid[23] = 120;
	/*
	 * RGB 4:4:4, the sRGB primaries, a native preferred timing and a
	 * frequency that is not continuous.
	 */
	edid[24] = 0x06;
	memcpy(edid + 25, srgb, sizeof(srgb));
	memset(edid + 38, 0x01, 16);

	vduse_guest_gpu_dtd(edid + 54, mode, &c->display_info);
	n = 1;
	list_for_each_entry(m, &c->modes, head) {
		if (n == VDUSE_GUEST_GPU_EDID_DTDS)
			break;
		if (m == mode ||
		    vduse_guest_gpu_mode(c, m->hdisplay, m->vdisplay) != m)
			continue;
		if (vduse_guest_gpu_dtd(edid + 54 + 18 * n, m,
					&c->display_info))
			n++;
	}
	/* A dummy descriptor fills an unused slot. */
	for (i = n; i < VDUSE_GUEST_GPU_EDID_DTDS; i++)
		edid[54 + 18 * i + 3] = 0x10;
	vduse_guest_gpu_edid_checksum(edid);
	return VDUSE_GUEST_GPU_EDID_BLOCK;
}

u32 vduse_guest_gpu_get_edid(struct vduse_guest_gpu *gpu,
			     const struct virtio_gpu_cmd_get_edid *cmd,
			     struct virtio_gpu_resp_edid *resp)
{
	struct vduse_guest_gpu_display *d = gpu->display;
	u32 id = le32_to_cpu(cmd->scanout);
	size_t size;

	memset(resp, 0, sizeof(*resp));
	if (!d || id >= d->nr_scanouts)
		return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;
	mutex_lock(&d->drm->mode_config.mutex);
	vduse_guest_gpu_probe(d, d->scanouts[id].connector);
	size = vduse_guest_gpu_edid(d->scanouts[id].connector, resp->edid);
	mutex_unlock(&d->drm->mode_config.mutex);
	if (!size)
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	resp->size = cpu_to_le32(size);
	return VIRTIO_GPU_RESP_OK_EDID;
}
