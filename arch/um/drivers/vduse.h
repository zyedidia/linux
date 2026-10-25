/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VDUSE_H
#define __UM_VDUSE_H

#include <linux/types.h>
#include <linux/vringh.h>

struct uml_vduse_dev;

/* The device that a backend describes at setup. */
struct uml_vduse_config {
	u32 device_id;
	u64 features;
	u32 num_queues;
	u16 queue_size;
	const void *config;
	u32 config_size;
};

/*
 * A device type. Setup runs once at boot and returns the backend's private
 * data, and release frees it if the device cannot be created. Start runs
 * when the host's driver sets DRIVER_OK, and stop when it resets the
 * device. After stop returns, the backend makes no more calls on the queues
 * until the next start. Kick can run in interrupt context.
 */
struct uml_vduse_backend_ops {
	const char *type;
	void *(*setup)(struct uml_vduse_dev *dev, char *opts,
		       struct uml_vduse_config *cfg);
	void (*release)(void *priv);
	int (*start)(void *priv);
	void (*stop)(void *priv);
	void (*kick)(void *priv, unsigned int index);
};

extern const struct uml_vduse_backend_ops uml_vduse_blk_ops;

const char *uml_vduse_name(struct uml_vduse_dev *dev);

int uml_vduse_getdesc(struct uml_vduse_dev *dev, unsigned int index,
		      struct vringh_kiov *riov, struct vringh_kiov *wiov,
		      u16 *head);
void uml_vduse_complete(struct uml_vduse_dev *dev, unsigned int index,
			u16 head, u32 len);
void uml_vduse_notify_disable(struct uml_vduse_dev *dev, unsigned int index);
bool uml_vduse_notify_enable(struct uml_vduse_dev *dev, unsigned int index);
bool uml_vduse_poll(struct uml_vduse_dev *dev, unsigned int index);

#endif /* __UM_VDUSE_H */
