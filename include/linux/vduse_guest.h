/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_VDUSE_GUEST_H
#define _LINUX_VDUSE_GUEST_H

#include <linux/list.h>
#include <linux/types.h>
#include <linux/vringh.h>

struct page;
struct pci_dev;
struct seq_file;
struct vduse_guest_dev;
struct vduse_guest_host;
struct vduse_guest_msg;

/* A shared memory region of a device, a nonzero multiple of the page size. */
struct vduse_guest_shm {
	u8 id;
	u64 size;
};

/*
 * A page-aligned range of a shared memory region and the memory behind it.
 * File is 0 for guest memory at physical address phys, or a file that
 * vduse_guest_shm_reg_bar() returned, at offset phys in it.
 */
struct vduse_guest_shm_range {
	u64 offset;
	u64 size;
	phys_addr_t phys;
	u32 file;
};

/*
 * The device that a backend describes at setup. Queue_sizes gives the size
 * of each of the num_queues queues, each a power of 2. Allow_reconnect lets
 * the device take over a device that an earlier guest left in use, and
 * without it setup fails when it finds one. A device with shared memory
 * regions, num_shm of them in shm, cannot reconnect.
 */
struct vduse_guest_config {
	u32 device_id;
	u64 features;
	u32 num_queues;
	const u16 *queue_sizes;
	bool allow_reconnect;
	const void *config;
	u32 config_size;
	const struct vduse_guest_shm *shm;
	u32 num_shm;
};

/*
 * A device type. Setup runs once at boot and returns the backend's private
 * data, and release frees it if the device cannot be created. Start runs
 * when the host's driver sets DRIVER_OK, and stop when it resets the
 * device. After stop returns, the backend makes no more calls on the queues
 * until the next start. Kick can run in interrupt context. Show, if set,
 * prints the backend's counters into /proc/vduse_guest. Shm_fault, if set,
 * takes a fault request on a shared memory region, which the backend answers
 * later with vduse_guest_shm_fault_done() from a context that can sleep.
 * Without it, fault requests are refused. Config_write, if set, takes a write
 * of the host's driver to the configuration space, which the backend applies
 * with vduse_guest_set_config() as the device requires. Without it, writes
 * are dropped.
 */
struct vduse_guest_backend_ops {
	const char *type;
	void *(*setup)(struct vduse_guest_dev *dev, char *opts,
		       struct vduse_guest_config *cfg);
	void (*release)(void *priv);
	int (*start)(void *priv);
	void (*stop)(void *priv);
	void (*kick)(void *priv, unsigned int index);
	void (*show)(void *priv, struct seq_file *m);
	void (*shm_fault)(void *priv, const struct vduse_guest_msg *msg);
	void (*config_write)(void *priv, u32 offset, const void *buf, u32 len);
};

extern const struct vduse_guest_backend_ops vduse_guest_blk_ops;
extern const struct vduse_guest_backend_ops vduse_guest_wlan_ops;
extern const struct vduse_guest_backend_ops vduse_guest_net_ops;
extern const struct vduse_guest_backend_ops vduse_guest_gpu_ops;

const char *vduse_guest_name(struct vduse_guest_dev *dev);
unsigned int vduse_guest_num_queues(struct vduse_guest_dev *dev);
u64 vduse_guest_driver_features(struct vduse_guest_dev *dev);
int vduse_guest_set_config(struct vduse_guest_dev *dev, u32 offset,
			   const void *buf, u32 len);
int vduse_guest_config_irq(struct vduse_guest_dev *dev);

int vduse_guest_getdesc(struct vduse_guest_dev *dev, unsigned int index,
			struct vringh_kiov *riov, struct vringh_kiov *wiov,
			u16 *head);
void vduse_guest_complete(struct vduse_guest_dev *dev, unsigned int index,
			  u16 head, u32 len);
void vduse_guest_used_elem(struct vduse_guest_dev *dev, unsigned int index,
			   struct vring_used_elem *elem, u16 head, u32 len);
void vduse_guest_complete_n(struct vduse_guest_dev *dev, unsigned int index,
			    const struct vring_used_elem *used, unsigned int n);
void vduse_guest_notify_disable(struct vduse_guest_dev *dev,
				unsigned int index);
bool vduse_guest_notify_enable(struct vduse_guest_dev *dev,
			       unsigned int index);
bool vduse_guest_poll(struct vduse_guest_dev *dev, unsigned int index);

/*
 * Copy len bytes at a device address into buf, from the bounce region or the
 * host's coherent allocations. An address outside them fails with -EFAULT.
 */
int vduse_guest_read(struct vduse_guest_dev *dev, u64 addr, void *buf,
		     size_t len);

/*
 * Map ranges of guest memory into a shared memory region, all or none, or
 * unmap the ranges within a range. When unmap returns, the host's mappings
 * of the range are gone, and the memory may be reused.
 */
int vduse_guest_shm_map(struct vduse_guest_dev *dev, u8 id,
			const struct vduse_guest_shm_range *ranges,
			unsigned int nr);
int vduse_guest_shm_unmap(struct vduse_guest_dev *dev, u8 id, u64 offset,
			  u64 size);
int vduse_guest_shm_reg_bar(struct vduse_guest_dev *dev, struct pci_dev *pdev,
			    int bar, u32 *file);
void vduse_guest_shm_fault_done(struct vduse_guest_dev *dev,
				const struct vduse_guest_msg *msg, bool ok);

enum {
	VDUSE_GUEST_MSG_GET_VQ_STATE,
	VDUSE_GUEST_MSG_SET_STATUS,
	VDUSE_GUEST_MSG_UPDATE_IOTLB,
	VDUSE_GUEST_MSG_SHM_FAULT,
	VDUSE_GUEST_MSG_SET_CONFIG,
	VDUSE_GUEST_MSG_UNKNOWN,
};

#define VDUSE_GUEST_CONFIG_WRITE_MAX 64

/*
 * A control message from the host. A shared memory fault carries the region
 * in index and the offset in start. A configuration write carries its offset
 * in start, its length in last and its bytes in data.
 */
struct vduse_guest_msg {
	int type;
	u32 request_id;
	u32 index;
	u8 status;
	u64 start;
	u64 last;
	u8 data[VDUSE_GUEST_CONFIG_WRITE_MAX];
};

struct vduse_guest_vq_info {
	u32 num;
	u64 desc_addr;
	u64 driver_addr;
	u64 device_addr;
	u16 avail_index;
	bool ready;
};

/* An inclusive range of IOVAs that the guest reaches through a pointer. */
struct vduse_guest_region {
	u64 start;
	u64 last;
	void *addr;
	bool readable;
	bool writable;
};

/*
 * The calls that a host provides to reach VDUSE. Each returns 0 or a
 * negative error unless it says otherwise.
 *
 * Alloc runs before the device's options are parsed, with the host that
 * serves the device, and keeps the host's data for the device. Free releases
 * it when the device cannot be created. Parse_opt returns 1 for an option of
 * the host's own and 0 for any other, and check_opts runs once every option
 * has been parsed.
 *
 * Create makes the device on the host, and fails with -EEXIST if one with
 * the same name exists. Destroy fails with -EBUSY while the device is in
 * use. Open and close connect the guest to the device. Node_id identifies
 * the host device, which is new each time it is created.
 *
 * Once enable_msgs returns, the host calls vduse_guest_msg_event() when
 * control messages arrive, until disable_msgs returns. Read_msg returns
 * -EAGAIN when there is none left.
 *
 * Set_config writes len bytes of the configuration space at offset, and
 * inject_config raises the configuration interrupt. Both are optional.
 *
 * Open_vq prepares a queue's kicks, which the host passes to
 * vduse_guest_kick() on the given CPU once enable_kick has handed them to
 * VDUSE. Each call gives the number of kicks that it covers, which is 0 for
 * a wakeup that found none. Sync_kick waits for calls of vduse_guest_kick()
 * that are running. Kick_pending, which is optional, returns 1 if a kick
 * has arrived that was not yet passed on, without taking it. Inject raises
 * the queue's interrupt on the host.
 *
 * Map gives the region of the IOVA space that holds an IOVA, and unmap
 * releases it. Map_umem prepares the chunks of guest pages that back the
 * bounce region, each of 1 << chunk_shift bytes, and reg_umem hands them to
 * VDUSE. Bounce_size fails with -EINVAL until the host has made its first
 * streaming map.
 *
 * Shm_setup declares a shared memory region before the host attaches the
 * device. Shm_map maps ranges of guest memory or of registered BARs into a
 * region, all or none, and shm_unmap unmaps the ranges within a range. They
 * are needed only for a device with regions. Shm_reg_bar, if set, registers
 * a passed-through PCI device's BAR, whose ranges the host maps
 * write-combined, and returns the file that names it, which is not 0. The
 * host unmaps the BAR's ranges while the PCI device's memory is disabled.
 *
 * Map_log maps the in-flight log, which outlives the guest. If its size
 * differs from the given one, it is resized when resize is set and is
 * otherwise an error, -EINVAL. Drop_log discards the log once the device
 * is destroyed.
 */
struct vduse_guest_host_ops {
	int (*alloc)(struct vduse_guest_dev *dev, struct vduse_guest_host *host);
	void (*free)(struct vduse_guest_dev *dev);
	int (*parse_opt)(struct vduse_guest_dev *dev, char *opt);
	int (*check_opts)(struct vduse_guest_dev *dev);

	int (*create)(struct vduse_guest_dev *dev,
		      const struct vduse_guest_config *cfg, u64 features,
		      u32 vq_align);
	int (*destroy)(struct vduse_guest_dev *dev);
	int (*open)(struct vduse_guest_dev *dev);
	void (*close)(struct vduse_guest_dev *dev);
	int (*node_id)(struct vduse_guest_dev *dev, u64 *node_dev,
		       u64 *node_ino);

	int (*enable_msgs)(struct vduse_guest_dev *dev);
	void (*disable_msgs)(struct vduse_guest_dev *dev);
	int (*read_msg)(struct vduse_guest_dev *dev, struct vduse_guest_msg *msg);
	int (*reply)(struct vduse_guest_dev *dev,
		     const struct vduse_guest_msg *msg, bool ok,
		     u16 avail_index);

	int (*get_features)(struct vduse_guest_dev *dev, u64 *features);
	int (*set_config)(struct vduse_guest_dev *dev, u32 offset,
			  const void *buf, u32 len);
	int (*inject_config)(struct vduse_guest_dev *dev);
	int (*vq_setup)(struct vduse_guest_dev *dev, unsigned int index,
			u16 max_size);
	int (*vq_get_info)(struct vduse_guest_dev *dev, unsigned int index,
			   struct vduse_guest_vq_info *info);
	int (*open_vq)(struct vduse_guest_dev *dev, unsigned int index,
		       unsigned int cpu);
	void (*close_vq)(struct vduse_guest_dev *dev, unsigned int index);
	int (*enable_kick)(struct vduse_guest_dev *dev, unsigned int index);
	void (*sync_kick)(struct vduse_guest_dev *dev, unsigned int index);
	int (*kick_pending)(struct vduse_guest_dev *dev, unsigned int index);
	int (*inject)(struct vduse_guest_dev *dev, unsigned int index);

	int (*map)(struct vduse_guest_dev *dev, u64 iova,
		   struct vduse_guest_region *region);
	void (*unmap)(struct vduse_guest_dev *dev,
		      struct vduse_guest_region *region);
	int (*bounce_size)(struct vduse_guest_dev *dev, u64 *size);
	int (*map_umem)(struct vduse_guest_dev *dev, struct page **chunks,
			unsigned int nr_chunks, unsigned int chunk_shift,
			u64 size);
	void (*unmap_umem)(struct vduse_guest_dev *dev);
	int (*reg_umem)(struct vduse_guest_dev *dev, u64 size);
	int (*dereg_umem)(struct vduse_guest_dev *dev, u64 size);

	int (*map_log)(struct vduse_guest_dev *dev, size_t size, bool resize,
		       void **addr);
	void (*unmap_log)(struct vduse_guest_dev *dev, void *addr, size_t size);
	void (*drop_log)(struct vduse_guest_dev *dev);

	int (*shm_setup)(struct vduse_guest_dev *dev, u8 id, u64 size);
	int (*shm_map)(struct vduse_guest_dev *dev, u8 id,
		       const struct vduse_guest_shm_range *ranges,
		       unsigned int nr);
	int (*shm_unmap)(struct vduse_guest_dev *dev, u8 id, u64 offset,
			 u64 size);
	int (*shm_reg_bar)(struct vduse_guest_dev *dev, struct pci_dev *pdev,
			   int bar, u32 *file);
};

/* A host, which serves the devices with its name, or every device. */
struct vduse_guest_host {
	const char *name;
	const struct vduse_guest_host_ops *ops;
	struct list_head list;
};

void vduse_guest_register_host(struct vduse_guest_host *host);

void *vduse_guest_host_data(struct vduse_guest_dev *dev);
void vduse_guest_set_host_data(struct vduse_guest_dev *dev, void *data);

void vduse_guest_msg_event(struct vduse_guest_dev *dev);
void vduse_guest_kick(struct vduse_guest_dev *dev, unsigned int index,
		      u64 count);

#endif /* _LINUX_VDUSE_GUEST_H */
