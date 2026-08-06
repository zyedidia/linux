/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umvduse: export a UML-driven device to the host over VDUSE.
 *
 * The transport creates /dev/vduse/$NAME on the host, services its
 * control messages and virtqueues, and hands descriptor chains to a
 * registered device shim. A shim knows nothing about VDUSE; the
 * transport knows nothing about what the bytes mean.
 *
 * Requests are delivered from a workqueue, so a shim may sleep and
 * allocate in ->handle(). Completion may come from any context.
 */

#ifndef __UMVDUSE_H__
#define __UMVDUSE_H__

#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/vringh.h>
#include <linux/workqueue.h>

#define UMVD_MAX_QUEUES		8
#define UMVD_MAX_CONFIG		256
#define UMVD_MAX_IOTLB		16
#define UMVD_NAME_LEN		128

struct umvd_dev;
struct umvd_queue;

/* One available descriptor chain handed to a shim. */
struct umvd_request {
	struct umvd_queue *q;
	u16 head;

	/* vringh naming: riov is device-readable (the host wrote it), wiov
	 * is device-writable (we fill it in). The iov_base entries have
	 * been translated from IOVAs to dereferenceable pointers by the
	 * time a shim sees them. */
	struct vringh_kiov riov;
	struct vringh_kiov wiov;
};

struct umvd_device_ops {
	const char *name;

	/*
	 * The shim moves data by page rather than by copy and therefore
	 * needs the umem region (umvd_umem_base()). Bring-up fails loudly
	 * if it cannot be provided.
	 */
	bool need_umem;

	/*
	 * Declare identity. Called once before the VDUSE device is
	 * created. Must fill in dev->device_id, dev->features,
	 * dev->num_queues, dev->queue_max[] and, if the device type has
	 * one, dev->config/dev->config_size. May sleep.
	 */
	int (*setup)(struct umvd_dev *dev);

	/* DRIVER_OK: negotiation finished, rings live. Optional. */
	int (*start)(struct umvd_dev *dev, u64 features);

	/*
	 * Handle one request. Ownership passes to the shim, which must
	 * eventually call umvd_request_complete(). Called from process
	 * context; may sleep.
	 */
	void (*handle)(struct umvd_dev *dev, struct umvd_request *req);

	/*
	 * Device reset (SET_STATUS 0). The shim must hand back every
	 * request it is holding by completing it -- the transport
	 * discards completions for a dead ring generation -- and stop
	 * submitting. In-flight hardware operations may finish later;
	 * their completions are discarded the same way. Optional.
	 */
	void (*reset)(struct umvd_dev *dev);

	void (*remove)(struct umvd_dev *dev);
};

struct umvd_iotlb_region {
	u64 start, last;
	void *va;
	bool writable;
	bool used;
};

struct umvd_dev {
	const struct umvd_device_ops *ops;
	void *priv;

	/* Filled in by ops->setup(). */
	u32 device_id;
	u32 vendor_id;
	u64 features;
	u32 num_queues;
	u16 queue_max[UMVD_MAX_QUEUES];
	u32 config_size;
	u8 config[UMVD_MAX_CONFIG];

	/* Filled in by the transport. */
	u64 negotiated;
	u8 status;
	bool started;

	/* Transport-internal state. */
	char name[UMVD_NAME_LEN];
	int ctrl_fd;
	int dev_fd;
	int dev_irq;
	struct umvd_queue *queues;
	struct work_struct msg_work;

	struct mutex iotlb_lock;
	struct umvd_iotlb_region iotlb[UMVD_MAX_IOTLB];

	void *umem;
	size_t umem_size;
	bool umem_live;
	bool umem_pinned;
	unsigned long umem_retry_at;
};

/*
 * Register a device shim. Only one shim is supported per UML instance;
 * a second registration fails with -EBUSY.
 */
int umvd_register_device(const struct umvd_device_ops *ops, void *priv);

/*
 * Finish a request. @written is the number of bytes placed into wiov and
 * becomes the used-ring length. @req is invalid on return. Any context.
 */
void umvd_request_complete(struct umvd_request *req, u32 written);

/* Queue a request was delivered on (0 .. num_queues - 1). */
u32 umvd_request_queue(const struct umvd_request *req);

/* Push a config-space change to the host (SET_CONFIG + config irq). */
int umvd_notify_config(struct umvd_dev *dev, u32 offset, u32 length);

/*
 * The umem region backing the VDUSE bounce domain, or NULL if it is not
 * live. IOVA i in [0, umvd_umem_size()) is umvd_umem_base() + i. The
 * region is vmalloc'd: use vmalloc_to_page() to get at its pages.
 */
static inline void *umvd_umem_base(struct umvd_dev *dev)
{
	return dev->umem_live ? dev->umem : NULL;
}

static inline size_t umvd_umem_size(struct umvd_dev *dev)
{
	return dev->umem_size;
}

#endif /* __UMVDUSE_H__ */
