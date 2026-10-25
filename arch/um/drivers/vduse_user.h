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

int uml_vduse_user_read_msg(int fd, struct uml_vduse_user_msg *msg);
int uml_vduse_user_reply(int fd, const struct uml_vduse_user_msg *msg,
			 int ok, uint16_t avail_index);

int uml_vduse_user_get_features(int fd, uint64_t *features);
int uml_vduse_user_vq_setup(int fd, uint32_t index, uint16_t max_size);
int uml_vduse_user_vq_get_info(int fd, uint32_t index,
			       struct uml_vduse_user_vq_info *info);
int uml_vduse_user_vq_set_kickfd(int fd, uint32_t index, int kickfd);
int uml_vduse_user_vq_inject_irq(int fd, uint32_t index);
int uml_vduse_user_create_kickfd(void);

int uml_vduse_user_map(int fd, uint64_t iova,
		       struct uml_vduse_user_region *region);
void uml_vduse_user_unmap(struct uml_vduse_user_region *region);

#endif /* __UM_VDUSE_USER_H */
