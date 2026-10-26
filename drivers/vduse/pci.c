// SPDX-License-Identifier: GPL-2.0
/*
 * The host of the VDUSE transport in a VM: a PCI device of the VMM that
 * gives the guest a VDUSE device on the host. BAR 4 is a window onto the
 * device's IOVA space, in which IOVA i lies at offset i. MSI-X vector 0
 * reports control messages and vector 1 + q each kick of queue q. A write
 * to a queue's doorbell in BAR 0 injects the queue's interrupt on the host.
 * Every other call is a command block in guest memory, whose physical
 * address the guest writes to the command register. The VMM runs the
 * command before the write returns.
 */

#define pr_fmt(fmt) "vduse-pci: " fmt

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vduse_guest.h>
#include <linux/vmalloc.h>
#include <uapi/linux/vduse.h>

#define VDUSE_PCI_VENDOR_ID 0x1af4
#define VDUSE_PCI_DEVICE_ID 0x10ff

#define VDUSE_PCI_REGS_BAR 0
#define VDUSE_PCI_WINDOW_BAR 4
#define VDUSE_PCI_REG_CMD 0x0
#define VDUSE_PCI_REG_DOORBELL 0x100

enum {
	VDUSE_PCI_CMD_NAME = 1,
	VDUSE_PCI_CMD_CREATE,
	VDUSE_PCI_CMD_DESTROY,
	VDUSE_PCI_CMD_OPEN,
	VDUSE_PCI_CMD_CLOSE,
	VDUSE_PCI_CMD_NODE_ID,
	VDUSE_PCI_CMD_ENABLE_MSGS,
	VDUSE_PCI_CMD_DISABLE_MSGS,
	VDUSE_PCI_CMD_READ_MSG,
	VDUSE_PCI_CMD_REPLY,
	VDUSE_PCI_CMD_GET_FEATURES,
	VDUSE_PCI_CMD_VQ_SETUP,
	VDUSE_PCI_CMD_VQ_GET_INFO,
	VDUSE_PCI_CMD_SET_KICK,
	VDUSE_PCI_CMD_MAP,
	VDUSE_PCI_CMD_UNMAP,
	VDUSE_PCI_CMD_IOVA_INFO,
	VDUSE_PCI_CMD_REG_UMEM,
	VDUSE_PCI_CMD_DEREG_UMEM,
};

/*
 * A command and its result, which is 0 or a negative error. The data holds
 * the VDUSE structure that the command passes, if any.
 */
struct vduse_pci_cmd {
	u32 op;
	s32 result;
	u64 args[6];
	u8 data[PAGE_SIZE - 56];
};

static_assert(sizeof(struct vduse_pci_cmd) == PAGE_SIZE);

struct vduse_pci;

struct vduse_pci_vq {
	struct vduse_pci *p;
	unsigned int index;
	int irq;
};

struct vduse_pci {
	struct pci_dev *pdev;
	void __iomem *regs;
	void *window;
	u64 window_size;
	char name[VDUSE_NAME_MAX];
	unsigned int max_queues;

	struct vduse_guest_host host;
	struct vduse_guest_dev *dev;

	/* The command block, one command at a time under cmd_lock. */
	struct mutex cmd_lock;
	struct vduse_pci_cmd *cmd;

	struct vduse_pci_vq *vqs;

	/* The guest physical addresses of the bounce region's chunks. */
	u64 *umem_chunks;
	unsigned int umem_nr_chunks;
	unsigned int umem_shift;
};

static struct vduse_pci *vduse_pci_of(struct vduse_guest_dev *dev)
{
	return vduse_guest_host_data(dev);
}

/* Clear the command block for a new command. The caller holds cmd_lock. */
static struct vduse_pci_cmd *vduse_pci_cmd_start(struct vduse_pci *p)
{
	lockdep_assert_held(&p->cmd_lock);
	memset(p->cmd, 0, offsetof(struct vduse_pci_cmd, data));
	return p->cmd;
}

static int vduse_pci_cmd_run(struct vduse_pci *p, u32 op)
{
	lockdep_assert_held(&p->cmd_lock);
	p->cmd->op = op;
	p->cmd->result = -EIO;
	writeq(virt_to_phys(p->cmd), p->regs + VDUSE_PCI_REG_CMD);
	return READ_ONCE(p->cmd->result);
}

/* Run a command that takes only arguments. */
static int vduse_pci_cmd_args(struct vduse_pci *p, u32 op, u64 arg0, u64 arg1)
{
	struct vduse_pci_cmd *cmd;

	guard(mutex)(&p->cmd_lock);
	cmd = vduse_pci_cmd_start(p);
	cmd->args[0] = arg0;
	cmd->args[1] = arg1;
	return vduse_pci_cmd_run(p, op);
}

static int vduse_pci_alloc(struct vduse_guest_dev *dev,
			   struct vduse_guest_host *host)
{
	struct vduse_pci *p = container_of(host, struct vduse_pci, host);

	if (p->dev)
		return -EBUSY;
	p->dev = dev;
	vduse_guest_set_host_data(dev, p);
	return 0;
}

static void vduse_pci_free(struct vduse_guest_dev *dev)
{
	struct vduse_pci *p = vduse_pci_of(dev);

	kfree(p->vqs);
	p->vqs = NULL;
	p->dev = NULL;
}

static int vduse_pci_parse_opt(struct vduse_guest_dev *dev, char *opt)
{
	return 0;
}

static int vduse_pci_check_opts(struct vduse_guest_dev *dev)
{
	return 0;
}

/* The VMM gives the device its own name. */
static int vduse_pci_create(struct vduse_guest_dev *dev,
			    const struct vduse_guest_config *cfg, u64 features,
			    u32 vq_align)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_dev_config *dc;
	struct vduse_pci_cmd *cmd;

	if (cfg->num_queues > p->max_queues)
		return -EINVAL;
	if (sizeof(*dc) + cfg->config_size > sizeof(cmd->data))
		return -E2BIG;

	guard(mutex)(&p->cmd_lock);
	cmd = vduse_pci_cmd_start(p);
	dc = (struct vduse_dev_config *)cmd->data;
	memset(dc, 0, sizeof(*dc));
	dc->device_id = cfg->device_id;
	dc->features = features;
	dc->vq_num = cfg->num_queues;
	dc->vq_align = vq_align;
	dc->config_size = cfg->config_size;
	memcpy(dc->config, cfg->config, cfg->config_size);
	return vduse_pci_cmd_run(p, VDUSE_PCI_CMD_CREATE);
}

static int vduse_pci_destroy(struct vduse_guest_dev *dev)
{
	return vduse_pci_cmd_args(vduse_pci_of(dev), VDUSE_PCI_CMD_DESTROY, 0, 0);
}

static int vduse_pci_open(struct vduse_guest_dev *dev)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	unsigned int i, n = vduse_guest_num_queues(dev);

	p->vqs = kzalloc_objs(*p->vqs, n);
	if (!p->vqs)
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		p->vqs[i].p = p;
		p->vqs[i].index = i;
		p->vqs[i].irq = pci_irq_vector(p->pdev, 1 + i);
	}

	return vduse_pci_cmd_args(p, VDUSE_PCI_CMD_OPEN, 0, 0);
}

static void vduse_pci_close(struct vduse_guest_dev *dev)
{
	vduse_pci_cmd_args(vduse_pci_of(dev), VDUSE_PCI_CMD_CLOSE, 0, 0);
}

static int vduse_pci_node_id(struct vduse_guest_dev *dev, u64 *node_dev,
			     u64 *node_ino)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	int err;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_NODE_ID);
	if (err)
		return err;
	*node_dev = p->cmd->args[0];
	*node_ino = p->cmd->args[1];
	return 0;
}

static irqreturn_t vduse_pci_msg_interrupt(int irq, void *opaque)
{
	struct vduse_pci *p = opaque;

	vduse_guest_msg_event(p->dev);
	return IRQ_HANDLED;
}

static int vduse_pci_enable_msgs(struct vduse_guest_dev *dev)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	int irq = pci_irq_vector(p->pdev, 0);
	int err;

	err = request_irq(irq, vduse_pci_msg_interrupt, 0, "vduse-pci", p);
	if (err)
		return err;

	err = vduse_pci_cmd_args(p, VDUSE_PCI_CMD_ENABLE_MSGS, 0, 0);
	if (err)
		free_irq(irq, p);
	return err;
}

static void vduse_pci_disable_msgs(struct vduse_guest_dev *dev)
{
	struct vduse_pci *p = vduse_pci_of(dev);

	vduse_pci_cmd_args(p, VDUSE_PCI_CMD_DISABLE_MSGS, 0, 0);
	free_irq(pci_irq_vector(p->pdev, 0), p);
}

static int vduse_pci_read_msg(struct vduse_guest_dev *dev,
			      struct vduse_guest_msg *msg)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_dev_request *req;
	int err;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_READ_MSG);
	if (err)
		return err;

	req = (struct vduse_dev_request *)p->cmd->data;
	memset(msg, 0, sizeof(*msg));
	msg->request_id = req->request_id;

	switch (req->type) {
	case VDUSE_GET_VQ_STATE:
		msg->type = VDUSE_GUEST_MSG_GET_VQ_STATE;
		msg->index = req->vq_state.index;
		break;
	case VDUSE_SET_STATUS:
		msg->type = VDUSE_GUEST_MSG_SET_STATUS;
		msg->status = req->s.status;
		break;
	case VDUSE_UPDATE_IOTLB:
		msg->type = VDUSE_GUEST_MSG_UPDATE_IOTLB;
		msg->start = req->iova.start;
		msg->last = req->iova.last;
		break;
	default:
		msg->type = VDUSE_GUEST_MSG_UNKNOWN;
		break;
	}

	return 0;
}

static int vduse_pci_reply(struct vduse_guest_dev *dev,
			   const struct vduse_guest_msg *msg, bool ok,
			   u16 avail_index)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_dev_response *resp;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	resp = (struct vduse_dev_response *)p->cmd->data;
	memset(resp, 0, sizeof(*resp));
	resp->request_id = msg->request_id;
	resp->result = ok ? VDUSE_REQ_RESULT_OK : VDUSE_REQ_RESULT_FAILED;
	if (msg->type == VDUSE_GUEST_MSG_GET_VQ_STATE) {
		resp->vq_state.index = msg->index;
		resp->vq_state.split.avail_index = avail_index;
	}
	return vduse_pci_cmd_run(p, VDUSE_PCI_CMD_REPLY);
}

static int vduse_pci_get_features(struct vduse_guest_dev *dev, u64 *features)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	int err;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_GET_FEATURES);
	if (!err)
		*features = p->cmd->args[0];
	return err;
}

static int vduse_pci_vq_setup(struct vduse_guest_dev *dev, unsigned int index,
			      u16 max_size)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_vq_config *vc;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	vc = (struct vduse_vq_config *)p->cmd->data;
	memset(vc, 0, sizeof(*vc));
	vc->index = index;
	vc->max_size = max_size;
	return vduse_pci_cmd_run(p, VDUSE_PCI_CMD_VQ_SETUP);
}

static int vduse_pci_vq_get_info(struct vduse_guest_dev *dev,
				 unsigned int index,
				 struct vduse_guest_vq_info *info)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_vq_info *vi;
	int err;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	vi = (struct vduse_vq_info *)p->cmd->data;
	memset(vi, 0, sizeof(*vi));
	vi->index = index;
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_VQ_GET_INFO);
	if (err)
		return err;

	info->num = vi->num;
	info->desc_addr = vi->desc_addr;
	info->driver_addr = vi->driver_addr;
	info->device_addr = vi->device_addr;
	info->avail_index = vi->split.avail_index;
	info->ready = vi->ready;
	return 0;
}

static irqreturn_t vduse_pci_kick_interrupt(int irq, void *opaque)
{
	struct vduse_pci_vq *vq = opaque;

	vduse_guest_kick(vq->p->dev, vq->index, 1);
	return IRQ_HANDLED;
}

static int vduse_pci_open_vq(struct vduse_guest_dev *dev, unsigned int index,
			     unsigned int cpu)
{
	struct vduse_pci_vq *vq = &vduse_pci_of(dev)->vqs[index];
	int err;

	err = request_irq(vq->irq, vduse_pci_kick_interrupt, 0, "vduse-pci",
			  vq);
	if (err)
		return err;

	irq_set_affinity(vq->irq, cpumask_of(cpu));
	return 0;
}

static void vduse_pci_close_vq(struct vduse_guest_dev *dev, unsigned int index)
{
	struct vduse_pci_vq *vq = &vduse_pci_of(dev)->vqs[index];

	free_irq(vq->irq, vq);
}

static int vduse_pci_enable_kick(struct vduse_guest_dev *dev,
				 unsigned int index)
{
	return vduse_pci_cmd_args(vduse_pci_of(dev), VDUSE_PCI_CMD_SET_KICK,
				  index, 0);
}

static void vduse_pci_sync_kick(struct vduse_guest_dev *dev,
				unsigned int index)
{
	synchronize_irq(vduse_pci_of(dev)->vqs[index].irq);
}

static int vduse_pci_inject(struct vduse_guest_dev *dev, unsigned int index)
{
	struct vduse_pci *p = vduse_pci_of(dev);

	writel(0, p->regs + VDUSE_PCI_REG_DOORBELL + 4 * index);
	return 0;
}

static int vduse_pci_map(struct vduse_guest_dev *dev, u64 iova,
			 struct vduse_guest_region *region)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_pci_cmd *cmd;
	int err;

	guard(mutex)(&p->cmd_lock);
	cmd = vduse_pci_cmd_start(p);
	cmd->args[0] = iova;
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_MAP);
	if (err)
		return err;
	if (cmd->args[1] >= p->window_size)
		return -EINVAL;

	region->start = cmd->args[0];
	region->last = cmd->args[1];
	region->addr = p->window + region->start;
	region->readable = cmd->args[2] & VDUSE_ACCESS_RO;
	region->writable = cmd->args[2] & VDUSE_ACCESS_WO;
	return 0;
}

static void vduse_pci_unmap(struct vduse_guest_dev *dev,
			    struct vduse_guest_region *region)
{
	vduse_pci_cmd_args(vduse_pci_of(dev), VDUSE_PCI_CMD_UNMAP,
			   region->start, region->last);
}

static int vduse_pci_bounce_size(struct vduse_guest_dev *dev, u64 *size)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_iova_info *ii;
	int err;

	guard(mutex)(&p->cmd_lock);
	vduse_pci_cmd_start(p);
	ii = (struct vduse_iova_info *)p->cmd->data;
	memset(ii, 0, sizeof(*ii));
	err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_IOVA_INFO);
	if (err)
		return err;
	if (ii->start != 0 || !(ii->capability & VDUSE_IOVA_CAP_UMEM))
		return -EOPNOTSUPP;

	*size = ii->last + 1;
	return 0;
}

static void vduse_pci_unmap_umem(struct vduse_guest_dev *dev)
{
	struct vduse_pci *p = vduse_pci_of(dev);

	kfree(p->umem_chunks);
	p->umem_chunks = NULL;
	p->umem_nr_chunks = 0;
}

/* The VMM maps the chunks when the region is registered. */
static int vduse_pci_map_umem(struct vduse_guest_dev *dev,
			      struct page **chunks, unsigned int nr_chunks,
			      unsigned int chunk_shift, u64 size)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	unsigned int i;

	if (nr_chunks * sizeof(u64) > sizeof(p->cmd->data))
		return -E2BIG;

	p->umem_chunks = kmalloc_objs(*p->umem_chunks, nr_chunks);
	if (!p->umem_chunks)
		return -ENOMEM;

	for (i = 0; i < nr_chunks; i++)
		p->umem_chunks[i] = page_to_phys(chunks[i]);
	p->umem_nr_chunks = nr_chunks;
	p->umem_shift = chunk_shift;
	return 0;
}

static int vduse_pci_reg_umem(struct vduse_guest_dev *dev, u64 size)
{
	struct vduse_pci *p = vduse_pci_of(dev);
	struct vduse_pci_cmd *cmd;

	guard(mutex)(&p->cmd_lock);
	cmd = vduse_pci_cmd_start(p);
	cmd->args[0] = size;
	cmd->args[1] = p->umem_shift;
	cmd->args[2] = p->umem_nr_chunks;
	memcpy(cmd->data, p->umem_chunks,
	       p->umem_nr_chunks * sizeof(*p->umem_chunks));
	return vduse_pci_cmd_run(p, VDUSE_PCI_CMD_REG_UMEM);
}

static int vduse_pci_dereg_umem(struct vduse_guest_dev *dev, u64 size)
{
	return vduse_pci_cmd_args(vduse_pci_of(dev), VDUSE_PCI_CMD_DEREG_UMEM,
				  size, 0);
}

/*
 * The log lives in guest memory, so it does not outlive the guest, and a
 * guest cannot reconnect to a device that an earlier guest left in use.
 */
static int vduse_pci_map_log(struct vduse_guest_dev *dev, size_t size,
			     bool resize, void **addr)
{
	*addr = vzalloc(size);
	return *addr ? 0 : -ENOMEM;
}

static void vduse_pci_unmap_log(struct vduse_guest_dev *dev, void *addr,
				size_t size)
{
	vfree(addr);
}

static void vduse_pci_drop_log(struct vduse_guest_dev *dev)
{
}

static const struct vduse_guest_host_ops vduse_pci_host_ops = {
	.alloc = vduse_pci_alloc,
	.free = vduse_pci_free,
	.parse_opt = vduse_pci_parse_opt,
	.check_opts = vduse_pci_check_opts,
	.create = vduse_pci_create,
	.destroy = vduse_pci_destroy,
	.open = vduse_pci_open,
	.close = vduse_pci_close,
	.node_id = vduse_pci_node_id,
	.enable_msgs = vduse_pci_enable_msgs,
	.disable_msgs = vduse_pci_disable_msgs,
	.read_msg = vduse_pci_read_msg,
	.reply = vduse_pci_reply,
	.get_features = vduse_pci_get_features,
	.vq_setup = vduse_pci_vq_setup,
	.vq_get_info = vduse_pci_vq_get_info,
	.open_vq = vduse_pci_open_vq,
	.close_vq = vduse_pci_close_vq,
	.enable_kick = vduse_pci_enable_kick,
	.sync_kick = vduse_pci_sync_kick,
	.inject = vduse_pci_inject,
	.map = vduse_pci_map,
	.unmap = vduse_pci_unmap,
	.bounce_size = vduse_pci_bounce_size,
	.map_umem = vduse_pci_map_umem,
	.unmap_umem = vduse_pci_unmap_umem,
	.reg_umem = vduse_pci_reg_umem,
	.dereg_umem = vduse_pci_dereg_umem,
	.map_log = vduse_pci_map_log,
	.unmap_log = vduse_pci_unmap_log,
	.drop_log = vduse_pci_drop_log,
};

static void vduse_pci_free_cmd(void *cmd)
{
	free_page((unsigned long)cmd);
}

static int vduse_pci_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct vduse_pci *p;
	int err, nvec;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->pdev = pdev;
	mutex_init(&p->cmd_lock);

	err = pcim_enable_device(pdev);
	if (err)
		return err;

	p->regs = pcim_iomap(pdev, VDUSE_PCI_REGS_BAR, 0);
	if (!p->regs)
		return -ENOMEM;

	p->window_size = pci_resource_len(pdev, VDUSE_PCI_WINDOW_BAR);
	p->window = devm_memremap(&pdev->dev,
				  pci_resource_start(pdev, VDUSE_PCI_WINDOW_BAR),
				  p->window_size, MEMREMAP_WB);
	if (IS_ERR(p->window))
		return PTR_ERR(p->window);

	p->cmd = (void *)get_zeroed_page(GFP_KERNEL);
	if (!p->cmd)
		return -ENOMEM;
	err = devm_add_action_or_reset(&pdev->dev, vduse_pci_free_cmd, p->cmd);
	if (err)
		return err;

	nvec = pci_msix_vec_count(pdev);
	if (nvec < 2)
		return -EINVAL;
	nvec = pci_alloc_irq_vectors(pdev, nvec, nvec, PCI_IRQ_MSIX);
	if (nvec < 0)
		return nvec;
	p->max_queues = nvec - 1;

	scoped_guard(mutex, &p->cmd_lock) {
		vduse_pci_cmd_start(p);
		err = vduse_pci_cmd_run(p, VDUSE_PCI_CMD_NAME);
		if (!err)
			strscpy(p->name, p->cmd->data, sizeof(p->name));
	}
	if (err)
		return err;

	p->host.name = p->name;
	p->host.ops = &vduse_pci_host_ops;
	vduse_guest_register_host(&p->host);

	pci_set_drvdata(pdev, p);
	dev_info(&pdev->dev, "serves /dev/vduse/%s with up to %u queues\n",
		 p->name, p->max_queues);
	return 0;
}

static const struct pci_device_id vduse_pci_ids[] = {
	{ PCI_DEVICE(VDUSE_PCI_VENDOR_ID, VDUSE_PCI_DEVICE_ID) },
	{}
};
MODULE_DEVICE_TABLE(pci, vduse_pci_ids);

/* A registered host cannot be removed, so the driver stays bound. */
static struct pci_driver vduse_pci_driver = {
	.name = "vduse-pci",
	.id_table = vduse_pci_ids,
	.probe = vduse_pci_probe,
	.driver.suppress_bind_attrs = true,
};
builtin_pci_driver(vduse_pci_driver);
