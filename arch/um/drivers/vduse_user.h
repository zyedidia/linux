/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_VDUSE_USER_H
#define __UM_VDUSE_USER_H

enum {
	UML_VDUSE_MSG_GET_VQ_STATE,
	UML_VDUSE_MSG_SET_STATUS,
	UML_VDUSE_MSG_UPDATE_IOTLB,
	UML_VDUSE_MSG_UNKNOWN,
};

/* A control message from the host. */
struct uml_vduse_user_msg {
	int type;
	uint32_t request_id;
	uint32_t index;
	uint8_t status;
	uint64_t start;
	uint64_t last;
};

struct uml_vduse_user_vq_info {
	uint32_t num;
	uint64_t desc_addr;
	uint64_t driver_addr;
	uint64_t device_addr;
	uint16_t avail_index;
	int ready;
};

/* An inclusive range of IOVAs mapped into the UML process. */
struct uml_vduse_user_region {
	uint64_t start;
	uint64_t last;
	void *addr;
	int readable;
	int writable;
};

int uml_vduse_user_open_control(void);
int uml_vduse_user_create(int control, const char *name, uint32_t device_id,
			  uint64_t features, uint32_t vq_num, uint32_t vq_align,
			  const void *config, uint32_t config_size);
int uml_vduse_user_destroy(int control, const char *name);
int uml_vduse_user_open(const char *name);
int uml_vduse_user_broker_create(int broker, const char *name,
				 uint32_t device_id, uint64_t features,
				 uint32_t vq_num, uint32_t vq_align,
				 const void *config, uint32_t config_size);
int uml_vduse_user_broker_destroy(int broker, const char *name);
int uml_vduse_user_broker_open(int broker, const char *name);
int uml_vduse_user_broker_resize_log(int broker, const char *name,
				     uint64_t size);

int uml_vduse_user_read_msg(int fd, struct uml_vduse_user_msg *msg);
int uml_vduse_user_reply(int fd, const struct uml_vduse_user_msg *msg,
			 int ok, uint16_t avail_index);

int uml_vduse_user_get_features(int fd, uint64_t *features);
int uml_vduse_user_set_config(int fd, uint32_t offset, const void *buf,
			      uint32_t len);
int uml_vduse_user_inject_config_irq(int fd);
int uml_vduse_user_vq_setup(int fd, uint32_t index, uint16_t max_size);
int uml_vduse_user_vq_get_info(int fd, uint32_t index,
			       struct uml_vduse_user_vq_info *info);
int uml_vduse_user_vq_set_kickfd(int fd, uint32_t index, int kickfd);
int uml_vduse_user_vq_inject_irq(int fd, uint32_t index);
int uml_vduse_user_create_kickfd(void);
int uml_vduse_user_kick_pending(int kickfd);

int uml_vduse_user_map(int fd, uint64_t iova,
		       struct uml_vduse_user_region *region);
void uml_vduse_user_unmap(struct uml_vduse_user_region *region);

int uml_vduse_user_node_id(int fd, uint64_t *dev, uint64_t *ino);
int uml_vduse_user_bounce_size(int fd, uint64_t *size);
void *uml_vduse_user_reserve_window(size_t size);
int uml_vduse_user_map_window(void *addr, size_t size, int fd,
			      unsigned long long offset);
void uml_vduse_user_unmap_window(void *addr, size_t size);
int uml_vduse_user_reg_umem(int fd, void *addr, uint64_t size);
int uml_vduse_user_dereg_umem(int fd, uint64_t size);

int uml_vduse_user_open_log(const char *path);
int uml_vduse_user_log_size(int fd, uint64_t *size);
int uml_vduse_user_map_log(int fd, size_t size, int resize, void **addr);
void uml_vduse_user_unmap_log(void *addr, size_t size);
int uml_vduse_user_remove_log(const char *path);
int uml_vduse_user_truncate_log(int fd);

#endif /* __UM_VDUSE_USER_H */
