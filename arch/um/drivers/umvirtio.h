/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umvirtio: generic transport between a UML device shim and a host bridge.
 *
 * A shim registers with umv_register_device() and is then handed request
 * chains as they arrive from the host. The transport knows nothing about
 * what the bytes mean; a shim knows nothing about how they got here.
 *
 * Requests are delivered from a workqueue, not from the IRQ handler, so a
 * shim may sleep and allocate in ->handle().
 */

#ifndef __UMVIRTIO_H__
#define __UMVIRTIO_H__

#include <linux/list.h>
#include <linux/types.h>
#include <linux/vringh.h>
#include <linux/workqueue.h>

#include "umvirtio_proto.h"
#include "umvirtio_user.h"

struct umv_dev;
struct umv_queue;

/* One available descriptor chain handed to a shim. */
struct umv_request {
	struct umv_queue *q;
	u16 head;

	/* vringh naming: riov is device-readable (host wrote it), wiov is
	 * device-writable (we fill it in). */
	struct vringh_kiov riov;
	struct vringh_kiov wiov;
};

struct umv_device_ops {
	const char *name;

	/*
	 * Declare identity. Called once before the handshake. Must fill in
	 * dev->device_id, dev->features, dev->num_queues and, if the device
	 * type has one, dev->config/dev->config_size.
	 */
	int (*setup)(struct umv_dev *dev);

	/* Feature negotiation finished. Optional. */
	int (*start)(struct umv_dev *dev, u64 features);

	/*
	 * Handle one request. Ownership passes to the shim, which must
	 * eventually call umv_request_complete(). Called from process
	 * context; may sleep.
	 */
	void (*handle)(struct umv_dev *dev, struct umv_request *req);

	void (*remove)(struct umv_dev *dev);
};

struct umv_dev {
	const struct umv_device_ops *ops;
	void *priv;

	/* Filled in by ops->setup(). */
	u32 device_id;
	u32 vendor_id;
	u64 features;
	u32 num_queues;
	u32 queue_size;
	u32 slot_size;
	u32 config_size;
	u8 config[UMV_MAX_CONFIG];

	/* Filled in by the transport. */
	u64 negotiated;
	bool started;
	struct umv_queue *queues;
	int sock_fd;
	int sock_irq;
	struct list_head node;
};

/*
 * Register a device shim. Only one shim is supported per UML instance;
 * a second registration fails with -EBUSY.
 */
int umv_register_device(const struct umv_device_ops *ops, void *priv);

/*
 * Finish a request. @written is the number of bytes placed into wiov and
 * becomes the used-ring length. @req is invalid on return.
 */
void umv_request_complete(struct umv_request *req, u32 written);

/* Push a config-space change to the host. */
int umv_notify_config(struct umv_dev *dev, u32 offset, u32 length);

#endif /* __UMVIRTIO_H__ */
