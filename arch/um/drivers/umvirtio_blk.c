// SPDX-License-Identifier: GPL-2.0
/*
 * umvirtio-blk: expose a UML-visible block device to the host bridge.
 *
 * The point of this shim is to sit in front of a driver UML owns for real
 * hardware -- an NVMe device passed through with CONFIG_UML_PCI_OVER_VFIO
 * -- and re-present it to the host as a virtio-blk device. It translates
 * virtio-blk requests into bios and nothing more; everything about how
 * those requests arrive is the transport's problem.
 *
 * There is no userspace, sysfs or udev in this configuration, so the disk
 * is named by major:minor on the command line rather than by path.
 */

#define pr_fmt(fmt) "umvirtio-blk: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/device/driver.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kdev_t.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <uapi/linux/virtio_blk.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_ids.h>

#include <init.h>

#include "umvirtio.h"

/*
 * Geometry. seg_max * size_max bounds a single request, and the slot has
 * to hold that plus the 16-byte header, so it gets a page of headroom.
 * Total UML memory spent on slots is 2 * queue_size * slot_size, which at
 * these numbers is 8 MiB -- keep that in mind against the UML mem= value.
 */
#define UMV_BLK_QUEUE_SIZE	32
#define UMV_BLK_SEG_MAX		32
#define UMV_BLK_SIZE_MAX	4096
#define UMV_BLK_SLOT_SIZE	(UMV_BLK_SEG_MAX * UMV_BLK_SIZE_MAX + PAGE_SIZE)

#define UMV_BLK_ID_BYTES	20

struct umv_blk {
	struct umv_dev *dev;
	struct file *bdev_file;
	struct block_device *bdev;
	bool readonly;
};

/* Per-bio state; the status byte lives in host-visible memory. */
struct umv_blk_io {
	struct umv_request *req;
	u8 *status;
	u32 used_len;
};

static int umv_blk_major = 259;		/* BLKEXT, where nvme namespaces land */
static int umv_blk_minor;
static char umv_blk_serial[UMV_BLK_ID_BYTES + 1] = "umvirtio";
static bool umv_blk_readonly;

static struct umv_blk umv_blk_dev;

/* ------------------------------------------------------------------ */
/* iov helpers. Entries are plain kernel pointers on the _kern path.    */
/* ------------------------------------------------------------------ */

static int umv_blk_pull(struct vringh_kiov *kiov, void *dst, size_t len)
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

static size_t umv_blk_iov_len(const struct vringh_kiov *kiov)
{
	size_t total = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++)
		total += kiov->iov[i].iov_len;

	return total;
}

static int umv_blk_add_iov(struct bio *bio, const struct vringh_kiov *kiov)
{
	unsigned int i;

	for (i = kiov->i; i < kiov->used; i++) {
		u8 *p = kiov->iov[i].iov_base;
		size_t len = kiov->iov[i].iov_len;

		/*
		 * Slots come from alloc_pages_exact() so they are physically
		 * contiguous, but split at page boundaries anyway rather than
		 * relying on that.
		 */
		while (len) {
			unsigned int off = offset_in_page(p);
			unsigned int chunk = min_t(size_t, len, PAGE_SIZE - off);

			if (!bio_add_page(bio, virt_to_page(p), chunk, off))
				return -EIO;

			p += chunk;
			len -= chunk;
		}
	}

	return 0;
}

/* ------------------------------------------------------------------ */
/* Request path                                                        */
/* ------------------------------------------------------------------ */

static void umv_blk_endio(struct bio *bio)
{
	struct umv_blk_io *io = bio->bi_private;

	*io->status = bio->bi_status == BLK_STS_OK ? VIRTIO_BLK_S_OK
						   : VIRTIO_BLK_S_IOERR;
	umv_request_complete(io->req, io->used_len);

	bio_put(bio);
	kfree(io);
}

static void umv_blk_fail(struct umv_request *req, u8 *status, u8 code)
{
	*status = code;
	umv_request_complete(req, 1);
}

static void umv_blk_submit(struct umv_blk *blk, struct umv_request *req,
			   u8 *status, blk_opf_t opf,
			   struct vringh_kiov *data, u64 offset, u32 used_len)
{
	unsigned short nvecs = (UMV_BLK_SLOT_SIZE >> PAGE_SHIFT) + 2;
	struct umv_blk_io *io;
	struct bio *bio;

	io = kzalloc(sizeof(*io), GFP_KERNEL);
	if (!io) {
		umv_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	bio = bio_alloc(blk->bdev, data ? nvecs : 0, opf, GFP_KERNEL);
	if (!bio) {
		kfree(io);
		umv_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	bio->bi_iter.bi_sector = offset >> SECTOR_SHIFT;

	if (data && umv_blk_add_iov(bio, data) < 0) {
		bio_put(bio);
		kfree(io);
		umv_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	io->req = req;
	io->status = status;
	io->used_len = used_len;

	bio->bi_private = io;
	bio->bi_end_io = umv_blk_endio;

	submit_bio(bio);
}

static void umv_blk_handle(struct umv_dev *dev, struct umv_request *req)
{
	struct umv_blk *blk = dev->priv;
	struct virtio_blk_outhdr hdr;
	struct kvec *last;
	size_t data_len;
	u8 *status;
	u64 offset;
	u32 type;

	/*
	 * The status byte is the final device-writable byte. Take it out of
	 * the data region before anything else looks at the iov.
	 */
	if (!req->wiov.used) {
		umv_request_complete(req, 0);
		return;
	}

	last = &req->wiov.iov[req->wiov.used - 1];
	if (!last->iov_len) {
		umv_request_complete(req, 0);
		return;
	}

	status = (u8 *)last->iov_base + last->iov_len - 1;
	last->iov_len--;

	if (umv_blk_pull(&req->riov, &hdr, sizeof(hdr)) < 0) {
		umv_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
		return;
	}

	type = le32_to_cpu(hdr.type);
	offset = (u64)le64_to_cpu(hdr.sector) << SECTOR_SHIFT;

	switch (type) {
	case VIRTIO_BLK_T_IN:
		data_len = umv_blk_iov_len(&req->wiov);
		umv_blk_submit(blk, req, status, REQ_OP_READ, &req->wiov,
			       offset, (u32)data_len + 1);
		return;

	case VIRTIO_BLK_T_OUT:
		if (blk->readonly) {
			umv_blk_fail(req, status, VIRTIO_BLK_S_IOERR);
			return;
		}
		umv_blk_submit(blk, req, status, REQ_OP_WRITE, &req->riov,
			       offset, 1);
		return;

	case VIRTIO_BLK_T_FLUSH:
		umv_blk_submit(blk, req, status,
			       REQ_OP_WRITE | REQ_PREFLUSH, NULL, 0, 1);
		return;

	case VIRTIO_BLK_T_GET_ID: {
		size_t n;

		if (!req->wiov.used || req->wiov.i >= req->wiov.used) {
			umv_blk_fail(req, status, VIRTIO_BLK_S_UNSUPP);
			return;
		}

		n = min_t(size_t, req->wiov.iov[req->wiov.i].iov_len,
			  UMV_BLK_ID_BYTES);
		memset(req->wiov.iov[req->wiov.i].iov_base, 0, n);
		memcpy(req->wiov.iov[req->wiov.i].iov_base, umv_blk_serial,
		       min(n, strlen(umv_blk_serial)));

		*status = VIRTIO_BLK_S_OK;
		umv_request_complete(req, (u32)n + 1);
		return;
	}

	default:
		umv_blk_fail(req, status, VIRTIO_BLK_S_UNSUPP);
		return;
	}
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

static int umv_blk_setup(struct umv_dev *dev)
{
	struct umv_blk *blk = dev->priv;
	struct virtio_blk_config cfg;
	blk_mode_t mode;
	u64 sectors;

	/*
	 * NVMe probes asynchronously, so the namespace may not exist yet
	 * even though we run at late_initcall.
	 */
	wait_for_device_probe();

	blk->readonly = umv_blk_readonly;
	mode = BLK_OPEN_READ | (blk->readonly ? 0 : BLK_OPEN_WRITE);

	blk->bdev_file = bdev_file_open_by_dev(MKDEV(umv_blk_major,
						     umv_blk_minor),
					       mode, blk, NULL);
	if (IS_ERR(blk->bdev_file)) {
		pr_err("cannot open %d:%d: %ld\n", umv_blk_major,
		       umv_blk_minor, PTR_ERR(blk->bdev_file));
		return PTR_ERR(blk->bdev_file);
	}

	blk->bdev = file_bdev(blk->bdev_file);
	blk->dev = dev;

	sectors = bdev_nr_sectors(blk->bdev);
	if (!sectors) {
		pr_err("%d:%d has zero capacity\n", umv_blk_major,
		       umv_blk_minor);
		return -EINVAL;
	}

	dev->device_id = VIRTIO_ID_BLOCK;
	dev->vendor_id = 0;
	dev->num_queues = 1;
	dev->queue_size = UMV_BLK_QUEUE_SIZE;
	dev->slot_size = UMV_BLK_SLOT_SIZE;

	dev->features = (1ULL << VIRTIO_F_VERSION_1) |
			(1ULL << VIRTIO_F_ACCESS_PLATFORM) |
			(1ULL << VIRTIO_BLK_F_SEG_MAX) |
			(1ULL << VIRTIO_BLK_F_SIZE_MAX) |
			(1ULL << VIRTIO_BLK_F_BLK_SIZE) |
			(1ULL << VIRTIO_BLK_F_FLUSH);
	if (blk->readonly)
		dev->features |= 1ULL << VIRTIO_BLK_F_RO;

	memset(&cfg, 0, sizeof(cfg));
	cfg.capacity = cpu_to_le64(sectors);
	cfg.seg_max = cpu_to_le32(UMV_BLK_SEG_MAX);
	cfg.size_max = cpu_to_le32(UMV_BLK_SIZE_MAX);
	cfg.blk_size = cpu_to_le32(bdev_logical_block_size(blk->bdev));
	cfg.num_queues = cpu_to_le16(1);

	memcpy(dev->config, &cfg, sizeof(cfg));
	dev->config_size = sizeof(cfg);

	pr_info("exporting %d:%d, %llu sectors, %u byte blocks%s\n",
		umv_blk_major, umv_blk_minor, sectors,
		bdev_logical_block_size(blk->bdev),
		blk->readonly ? " (read-only)" : "");

	return 0;
}

static void umv_blk_remove(struct umv_dev *dev)
{
	struct umv_blk *blk = dev->priv;

	if (blk->bdev_file && !IS_ERR(blk->bdev_file)) {
		fput(blk->bdev_file);
		blk->bdev_file = NULL;
		blk->bdev = NULL;
	}
}

static const struct umv_device_ops umv_blk_ops = {
	.name = "umvirtio-blk",
	.setup = umv_blk_setup,
	.handle = umv_blk_handle,
	.remove = umv_blk_remove,
};

static int __init umv_blk_init(void)
{
	return umv_register_device(&umv_blk_ops, &umv_blk_dev);
}
late_initcall(umv_blk_init);

/* ------------------------------------------------------------------ */
/* Parameters                                                          */
/* ------------------------------------------------------------------ */

static int umv_blk_disk_set(const char *str, const struct kernel_param *kp)
{
	int major, minor;

	if (sscanf(str, "%d:%d", &major, &minor) != 2)
		return -EINVAL;
	if (major <= 0 || minor < 0)
		return -EINVAL;

	umv_blk_major = major;
	umv_blk_minor = minor;

	return 0;
}

static int umv_blk_disk_get(char *buffer, const struct kernel_param *kp)
{
	return scnprintf(buffer, PAGE_SIZE, "%d:%d\n", umv_blk_major,
			 umv_blk_minor);
}

static const struct kernel_param_ops umv_blk_disk_param_ops = {
	.set = umv_blk_disk_set,
	.get = umv_blk_disk_get,
};

device_param_cb(disk, &umv_blk_disk_param_ops, NULL, 0400);
__uml_help(umv_blk_disk_param_ops,
"umvirtio_blk.disk=<major>:<minor>\n"
"    Block device to export to the host, by device number. There is no\n"
"    sysfs or udev in this configuration, so a path cannot be used. NVMe\n"
"    namespaces are normally 259:0 upwards, which is the default.\n\n"
);

module_param_string(serial, umv_blk_serial, sizeof(umv_blk_serial), 0400);
__uml_help(umv_blk_serial,
"umvirtio_blk.serial=<string>\n"
"    Serial reported for VIRTIO_BLK_T_GET_ID, truncated to 20 bytes.\n\n"
);

module_param_named(readonly, umv_blk_readonly, bool, 0400);
__uml_help(umv_blk_readonly,
"umvirtio_blk.readonly=1\n"
"    Open the disk read-only and offer VIRTIO_BLK_F_RO, so the host\n"
"    cannot write through to the hardware.\n\n"
);
