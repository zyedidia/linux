// SPDX-License-Identifier: GPL-2.0
/*
 * umvduse-blk: expose a UML-visible block device to the host as
 * virtio-blk.
 *
 * The point of this shim is to sit in front of a driver UML owns for
 * real hardware -- an NVMe device passed through with
 * CONFIG_UML_PCI_OVER_VFIO -- and re-present it to the host. It
 * translates virtio-blk requests into bios and nothing more; everything
 * about how those requests arrive is the transport's problem.
 *
 * Data buffers land in the transport's umem region (the memory backing
 * the VDUSE bounce domain), which is page-backed UML memory: bios
 * reference those pages directly, so the device DMAs straight from/to
 * the same memory the host bounces into. The shim refuses to run
 * without umem rather than grow a second copy.
 *
 * A request may span more descriptors and pages than one bio can carry
 * (the host bounds requests only by seg_max * size_max), so data is
 * submitted as a batch of bios sharing one refcounted completion.
 *
 * There is no userspace, sysfs or udev in this configuration, so the
 * disk is named by major:minor on the command line rather than by path.
 */

#define pr_fmt(fmt) "umvduse-blk: " fmt

#include <linux/atomic.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <uapi/linux/virtio_blk.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ids.h>

#include <init.h>

#include "umvduse.h"

#define UMVD_BLK_ID_BYTES	20

struct umvd_blk {
	struct umvd_dev *dev;
	struct file *bdev_file;
	struct block_device *bdev;
	bool readonly;
};

/* One virtio request, fanned out over one or more bios. The final
 * put writes the status byte (which lives in host-visible memory) and
 * completes the request. */
struct umvd_blk_io {
	struct umvd_request *req;
	u8 *status;
	u32 used_len;
	atomic_t pending;
	bool failed;
};

static int umvd_blk_major = 259;	/* BLKEXT, where nvme namespaces land */
static int umvd_blk_minor;
static char umvd_blk_serial[UMVD_BLK_ID_BYTES + 1] = "umvduse";
static bool umvd_blk_readonly;
static unsigned int umvd_blk_wait_ms = 10000;
static unsigned int umvd_blk_queue_max = 256;
static unsigned int umvd_blk_size_max = 131072;

static struct umvd_blk umvd_blk_dev;

/* ------------------------------------------------------------------ */
/* iov helpers. Entries are translated pointers by the time we run.     */
/* ------------------------------------------------------------------ */

static int umvd_blk_pull(struct vringh_kiov *kiov, void *dst, size_t len)
{
	u8 *out = dst;

	while (len) {
		struct kvec *v;
		size_t n;

		if (kiov->i >= kiov->used)
			return -EINVAL;

		v = &kiov->iov[kiov->i];
		n = min(len, v->iov_len);
		memcpy(out, v->iov_base, n);

		out += n;
		len -= n;
		v->iov_base = (u8 *)v->iov_base + n;
		v->iov_len -= n;
		if (!v->iov_len)
			kiov->i++;
	}

	return 0;
}

static size_t umvd_blk_iov_len(const struct vringh_kiov *kiov)
{
	size_t total = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++)
		total += kiov->iov[i].iov_len;

	return total;
}

/* ------------------------------------------------------------------ */
/* Request path                                                        */
/* ------------------------------------------------------------------ */

static void umvd_blk_io_put(struct umvd_blk_io *io)
{
	if (!atomic_dec_and_test(&io->pending))
		return;

	*io->status = io->failed ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;
	umvd_request_complete(io->req, io->used_len);
	kfree(io);
}

static void umvd_blk_endio(struct bio *bio)
{
	struct umvd_blk_io *io = bio->bi_private;

	if (bio->bi_status != BLK_STS_OK)
		io->failed = true;

	bio_put(bio);
	umvd_blk_io_put(io);
}

static void umvd_blk_fail(struct umvd_request *req, u8 *status, u8 code)
{
	*status = code;
	umvd_request_complete(req, 1);
}

/* Data buffers must sit in the umem region: that is the only translated
 * memory with pages behind it, and the whole bounce domain is backed by
 * it while the transport is running. */
static struct page *umvd_blk_page(struct umvd_blk *blk, void *p)
{
	void *base = umvd_umem_base(blk->dev);

	if (!base || p < base || p >= base + umvd_umem_size(blk->dev))
		return NULL;

	return vmalloc_to_page(p);
}

/*
 * Submit the data phase of a request as one or more bios against
 * consecutive disk offsets. io->pending carries a bias reference so the
 * request cannot complete before every bio has been submitted.
 */
static void umvd_blk_submit_data(struct umvd_blk *blk, struct umvd_blk_io *io,
				 blk_opf_t opf, struct vringh_kiov *data,
				 u64 offset)
{
	size_t total = umvd_blk_iov_len(data);
	size_t remaining = total;
	struct bio *bio = NULL;
	size_t bio_bytes = 0;
	unsigned int i;

	for (i = data->i; i < data->used; i++) {
		u8 *p = data->iov[i].iov_base;
		size_t len = data->iov[i].iov_len;

		while (len) {
			unsigned int off = offset_in_page(p);
			unsigned int chunk = min_t(size_t, len,
						   PAGE_SIZE - off);
			struct page *page;

			page = umvd_blk_page(blk, p);
			if (!page) {
				pr_err_ratelimited("data buffer outside umem\n");
				goto fail;
			}

			if (!bio) {
				unsigned int nvecs;

				nvecs = min_t(size_t, BIO_MAX_VECS,
					      DIV_ROUND_UP(remaining,
							   PAGE_SIZE) + 1);
				bio = bio_alloc(blk->bdev, nvecs, opf,
						GFP_KERNEL);
				bio->bi_iter.bi_sector =
					(offset + (total - remaining))
						>> SECTOR_SHIFT;
				bio->bi_private = io;
				bio->bi_end_io = umvd_blk_endio;
				bio_bytes = 0;
			}

			if (!bio_add_page(bio, page, chunk, off)) {
				/* Full: submit and put the rest in the
				 * next one. Splitting mid-sector would
				 * corrupt; it cannot happen for the
				 * page-aligned buffers real drivers
				 * build, so reject rather than handle. */
				if (bio_bytes & (SECTOR_SIZE - 1)) {
					pr_err_ratelimited("unaligned bio split\n");
					bio_put(bio);
					goto fail;
				}
				atomic_inc(&io->pending);
				submit_bio(bio);
				bio = NULL;
				continue;
			}

			bio_bytes += chunk;
			remaining -= chunk;
			p += chunk;
			len -= chunk;
		}
	}

	if (bio) {
		atomic_inc(&io->pending);
		submit_bio(bio);
	}
	return;

fail:
	io->failed = true;
	if (bio) {
		/* Unsubmitted bio: drop it without completing io. */
		bio->bi_end_io = NULL;
		bio_put(bio);
	}
}

static void umvd_blk_handle(struct umvd_dev *dev, struct umvd_request *req)
{
	struct umvd_blk *blk = dev->priv;
	struct virtio_blk_outhdr hdr;
	struct umvd_blk_io *io;
	struct vringh_kiov *data;
	struct kvec *last;
	size_t data_len;
	blk_opf_t opf;
	u8 *status;
	u64 offset;
	u32 type;

	/*
	 * The status byte is the final device-writable byte. Take it out
	 * of the data region before anything else looks at the iov.
	 */
	if (!req->wiov.used) {
		umvd_request_complete(req, 0);
		return;
	}
	last = &req->wiov.iov[req->wiov.used - 1];
	if (!last->iov_len) {
		umvd_request_complete(req, 0);
		return;
	}
	status = (u8 *)last->iov_base + last->iov_len - 1;
	last->iov_len--;

	if (umvd_blk_pull(&req->riov, &hdr, sizeof(hdr)) < 0) {
		umvd_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	type = le32_to_cpu((__force __le32)hdr.type);
	offset = (u64)le64_to_cpu((__force __le64)hdr.sector) << SECTOR_SHIFT;

	switch (type) {
	case VIRTIO_BLK_T_IN:
		opf = REQ_OP_READ;
		data = &req->wiov;
		break;

	case VIRTIO_BLK_T_OUT:
		if (blk->readonly) {
			umvd_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
			return;
		}
		opf = REQ_OP_WRITE;
		data = &req->riov;
		break;

	case VIRTIO_BLK_T_FLUSH:
		opf = REQ_OP_WRITE | REQ_PREFLUSH;
		data = NULL;
		break;

	case VIRTIO_BLK_T_GET_ID: {
		size_t n = 0;

		if (req->wiov.i < req->wiov.used) {
			struct kvec *v = &req->wiov.iov[req->wiov.i];

			n = min_t(size_t, v->iov_len, UMVD_BLK_ID_BYTES);
			memset(v->iov_base, 0, n);
			memcpy(v->iov_base, umvd_blk_serial,
			       min(n, strlen(umvd_blk_serial)));
		}

		*status = VIRTIO_BLK_S_OK;
		umvd_request_complete(req, (u32)n + 1);
		return;
	}

	default:
		umvd_blk_fail(req, status, VIRTIO_BLK_S_UNSUPP);
		return;
	}

	data_len = data ? umvd_blk_iov_len(data) : 0;
	if (data_len & (SECTOR_SIZE - 1)) {
		umvd_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	/* A transfer with no data only makes sense for flush. */
	if (type != VIRTIO_BLK_T_FLUSH && !data_len) {
		umvd_blk_fail(req, status, VIRTIO_BLK_S_OK);
		return;
	}

	io = kzalloc(sizeof(*io), GFP_KERNEL);
	if (!io) {
		umvd_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}
	io->req = req;
	io->status = status;
	io->used_len = (type == VIRTIO_BLK_T_IN ? (u32)data_len : 0) + 1;
	atomic_set(&io->pending, 1);	/* submitter reference */

	if (!data || !data_len) {
		struct bio *bio;

		bio = bio_alloc(blk->bdev, 0, opf, GFP_KERNEL);
		bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;
		bio->bi_private = io;
		bio->bi_end_io = umvd_blk_endio;
		atomic_inc(&io->pending);
		submit_bio(bio);
	} else {
		umvd_blk_submit_data(blk, io, opf, data, offset);
	}

	umvd_blk_io_put(io);	/* drop the submitter reference */
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static int umvd_blk_setup(struct umvd_dev *dev)
{
	struct umvd_blk *blk = dev->priv;
	struct virtio_blk_config cfg;
	unsigned long deadline;
	blk_mode_t mode;
	u64 sectors;

	blk->readonly = umvd_blk_readonly;
	mode = BLK_OPEN_READ | (blk->readonly ? 0 : BLK_OPEN_WRITE);

	/*
	 * NVMe probes asynchronously and scans its namespaces from a
	 * workqueue, so the disk may not exist yet even though we run at
	 * late_initcall. Poll for it, bounded by umvduse_blk.wait_ms=.
	 */
	deadline = jiffies + msecs_to_jiffies(umvd_blk_wait_ms);
	for (;;) {
		wait_for_device_probe();
		blk->bdev_file = bdev_file_open_by_dev(MKDEV(umvd_blk_major,
							     umvd_blk_minor),
						       mode, blk, NULL);
		if (!IS_ERR(blk->bdev_file))
			break;
		if (time_after(jiffies, deadline)) {
			pr_err("cannot open %d:%d: %ld\n", umvd_blk_major,
			       umvd_blk_minor, PTR_ERR(blk->bdev_file));
			return PTR_ERR(blk->bdev_file);
		}
		msleep(100);
	}

	blk->bdev = file_bdev(blk->bdev_file);
	blk->dev = dev;

	sectors = bdev_nr_sectors(blk->bdev);
	if (!sectors) {
		pr_err("%d:%d has zero capacity\n", umvd_blk_major,
		       umvd_blk_minor);
		return -EINVAL;
	}

	dev->device_id = VIRTIO_ID_BLOCK;
	dev->vendor_id = 0;
	dev->num_queues = 1;
	dev->queue_max[0] = umvd_blk_queue_max;

	/* CONFIG_WCE must not be offered: VDUSE rejects it (it would need
	 * a writable config space). */
	dev->features = (1ULL << VIRTIO_BLK_F_SEG_MAX) |
			(1ULL << VIRTIO_BLK_F_SIZE_MAX) |
			(1ULL << VIRTIO_BLK_F_BLK_SIZE) |
			(1ULL << VIRTIO_BLK_F_FLUSH);
	if (blk->readonly)
		dev->features |= 1ULL << VIRTIO_BLK_F_RO;

	memset(&cfg, 0, sizeof(cfg));
	cfg.capacity = (__force __virtio64)cpu_to_le64(sectors);
	cfg.seg_max = (__force __virtio32)
		cpu_to_le32(umvd_blk_queue_max - 2);
	cfg.size_max = (__force __virtio32)cpu_to_le32(umvd_blk_size_max);
	cfg.blk_size = (__force __virtio32)
		cpu_to_le32(bdev_logical_block_size(blk->bdev));
	cfg.num_queues = (__force __virtio16)cpu_to_le16(1);

	memcpy(dev->config, &cfg, sizeof(cfg));
	dev->config_size = sizeof(cfg);

	pr_info("exporting %d:%d, %llu sectors, %u byte blocks%s\n",
		umvd_blk_major, umvd_blk_minor, sectors,
		bdev_logical_block_size(blk->bdev),
		blk->readonly ? " (read-only)" : "");

	return 0;
}

/*
 * Device reset. Nothing is parked here: every accepted request has bios
 * in flight, and their completions land in a dead ring generation that
 * the transport discards. The disk stays open across resets.
 */
static void umvd_blk_reset(struct umvd_dev *dev)
{
}

static void umvd_blk_remove(struct umvd_dev *dev)
{
	struct umvd_blk *blk = dev->priv;

	if (blk->bdev_file && !IS_ERR(blk->bdev_file)) {
		fput(blk->bdev_file);
		blk->bdev_file = NULL;
		blk->bdev = NULL;
	}
}

static const struct umvd_device_ops umvd_blk_ops = {
	.name = "umvduse-blk",
	.need_umem = true,
	.setup = umvd_blk_setup,
	.handle = umvd_blk_handle,
	.reset = umvd_blk_reset,
	.remove = umvd_blk_remove,
};

static int __init umvd_blk_init(void)
{
	return umvd_register_device(&umvd_blk_ops, &umvd_blk_dev);
}
late_initcall(umvd_blk_init);

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

static int umvd_blk_disk_set(const char *str, const struct kernel_param *kp)
{
	int major, minor;

	if (sscanf(str, "%d:%d", &major, &minor) != 2)
		return -EINVAL;
	if (major <= 0 || minor < 0)
		return -EINVAL;

	umvd_blk_major = major;
	umvd_blk_minor = minor;

	return 0;
}

static int umvd_blk_disk_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d:%d\n", umvd_blk_major,
			 umvd_blk_minor);
}

static const struct kernel_param_ops umvd_blk_disk_param_ops = {
	.set = umvd_blk_disk_set,
	.get = umvd_blk_disk_get,
};

device_param_cb(disk, &umvd_blk_disk_param_ops, NULL, 0400);
__uml_help(umvd_blk_disk_param_ops,
"umvduse_blk.disk=<major>:<minor>\n"
"    Block device to export to the host, by device number. There is no\n"
"    sysfs or udev in this configuration, so a path cannot be used. NVMe\n"
"    namespaces are normally 259:0 upwards, which is the default.\n\n"
);

module_param_string(serial, umvd_blk_serial, sizeof(umvd_blk_serial), 0400);
__uml_help(umvd_blk_serial,
"umvduse_blk.serial=<string>\n"
"    Serial reported for VIRTIO_BLK_T_GET_ID, truncated to 20 bytes.\n\n"
);

module_param_named(readonly, umvd_blk_readonly, bool, 0400);
__uml_help(umvd_blk_readonly,
"umvduse_blk.readonly=1\n"
"    Open the disk read-only and offer VIRTIO_BLK_F_RO, so the host\n"
"    cannot write through to the hardware.\n\n"
);

module_param_named(wait_ms, umvd_blk_wait_ms, uint, 0400);
__uml_help(umvd_blk_wait_ms,
"umvduse_blk.wait_ms=<n>\n"
"    How long to wait for the exported disk to show up before giving\n"
"    up, in milliseconds (default 10000). NVMe namespaces appear\n"
"    asynchronously some time into boot.\n\n"
);

module_param_named(queue_max, umvd_blk_queue_max, uint, 0400);
__uml_help(umvd_blk_queue_max,
"umvduse_blk.queue_max=<n>\n"
"    Virtqueue size to offer (default 256; power of two, 4..1024).\n"
"    seg_max is derived as queue_max - 2.\n\n"
);

module_param_named(size_max, umvd_blk_size_max, uint, 0400);
__uml_help(umvd_blk_size_max,
"umvduse_blk.size_max=<bytes>\n"
"    Maximum size of a single request segment (default 131072).\n\n"
);
