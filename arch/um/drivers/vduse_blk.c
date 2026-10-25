// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "vduse-uml-blk: " fmt

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uio.h>
#include <linux/virtio_byteorder.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <uapi/linux/virtio_blk.h>
#include <uapi/linux/virtio_ids.h>

#include "vduse.h"

#define UML_VDUSE_BLK_QUEUE_SIZE 256
#define UML_VDUSE_BLK_INLINE_IOVS 4
#define UML_VDUSE_BLK_RETRY_DELAY msecs_to_jiffies(10)

struct uml_vduse_blk {
	struct uml_vduse_dev *dev;
	struct file *bdev_file;
	struct block_device *bdev;
	struct virtio_blk_config config;
	char id[VIRTIO_BLK_ID_BYTES];

	struct delayed_work work;
	atomic_t inflight;
	wait_queue_head_t idle;
};

struct uml_vduse_blk_req {
	struct uml_vduse_blk *blk;
	u16 head;
	u32 type;
	u8 *status;

	struct vringh_kiov riov;
	struct vringh_kiov wiov;
	struct kvec rkvec[UML_VDUSE_BLK_INLINE_IOVS];
	struct kvec wkvec[UML_VDUSE_BLK_INLINE_IOVS];
	/* The data buffers, without the header and the status byte. */
	struct iov_iter data;

	struct page **pages;
	unsigned int nr_pages;
};

static struct uml_vduse_blk_req *uml_vduse_blk_alloc(struct uml_vduse_blk *blk)
{
	struct uml_vduse_blk_req *req;

	req = kzalloc_obj(*req);
	if (!req)
		return NULL;

	req->blk = blk;
	vringh_kiov_init(&req->riov, req->rkvec, ARRAY_SIZE(req->rkvec));
	vringh_kiov_init(&req->wiov, req->wkvec, ARRAY_SIZE(req->wkvec));
	return req;
}

static void uml_vduse_blk_free(struct uml_vduse_blk_req *req)
{
	unsigned int i;

	for (i = 0; i < req->nr_pages; i++) {
		if (req->pages[i])
			__free_page(req->pages[i]);
	}
	kfree(req->pages);
	vringh_kiov_cleanup(&req->riov);
	vringh_kiov_cleanup(&req->wiov);
	kfree(req);
}

static void uml_vduse_blk_finish(struct uml_vduse_blk_req *req, u8 status,
				 u32 len)
{
	struct uml_vduse_blk *blk = req->blk;

	if (req->status)
		*req->status = status;
	uml_vduse_complete(blk->dev, 0, req->head, len);
	uml_vduse_blk_free(req);

	if (atomic_dec_and_test(&blk->inflight))
		wake_up(&blk->idle);
}

/* Copy between the request's data buffers and its pages. */
static bool uml_vduse_blk_copy(struct uml_vduse_blk_req *req, bool to_pages)
{
	size_t left = iov_iter_count(&req->data);
	unsigned int i;

	for (i = 0; i < req->nr_pages; i++) {
		size_t n = min_t(size_t, left, PAGE_SIZE);
		void *p = page_address(req->pages[i]);

		if ((to_pages ? copy_from_iter(p, n, &req->data) :
				copy_to_iter(p, n, &req->data)) != n)
			return false;
		left -= n;
	}

	return true;
}

static void uml_vduse_blk_end_io(struct bio *bio)
{
	struct uml_vduse_blk_req *req = bio->bi_private;
	u8 status = VIRTIO_BLK_S_OK;
	u32 len = 1;

	if (bio->bi_status) {
		status = VIRTIO_BLK_S_IOERR;
	} else if (req->type == VIRTIO_BLK_T_IN) {
		len += iov_iter_count(&req->data);
		if (!uml_vduse_blk_copy(req, false))
			status = VIRTIO_BLK_S_IOERR;
	}

	bio_put(bio);
	uml_vduse_blk_finish(req, status, len);
}

static int uml_vduse_blk_alloc_pages(struct uml_vduse_blk_req *req)
{
	size_t len = iov_iter_count(&req->data);
	unsigned int i;

	req->nr_pages = DIV_ROUND_UP(len, PAGE_SIZE);
	req->pages = kzalloc_objs(*req->pages, req->nr_pages);
	if (!req->pages) {
		req->nr_pages = 0;
		return -ENOMEM;
	}

	for (i = 0; i < req->nr_pages; i++) {
		req->pages[i] = alloc_page(GFP_KERNEL);
		if (!req->pages[i])
			return -ENOMEM;
	}

	return 0;
}

/*
 * Whether every data buffer of a request lies in guest pages, which the
 * transport gives when guest pages back the bounce region, and is aligned to
 * the disk's logical block size.
 */
static bool uml_vduse_blk_in_pages(struct uml_vduse_blk_req *req)
{
	unsigned int lbs = bdev_logical_block_size(req->blk->bdev);
	const struct iov_iter *it = &req->data;
	size_t left = iov_iter_count(it), off = it->iov_offset;
	const struct kvec *kv = it->kvec;

	for (; left; kv++, off = 0) {
		size_t n = min(kv->iov_len - off, left);
		void *p = kv->iov_base + off;

		if (!n)
			continue;
		if (!virt_addr_valid(p) || !virt_addr_valid(p + n - 1) ||
		    !IS_ALIGNED((unsigned long)p, lbs) || !IS_ALIGNED(n, lbs))
			return false;
		left -= n;
	}
	return true;
}

/* Submit a request whose data buffers lie in guest pages, without a copy. */
static void uml_vduse_blk_rw_pages(struct uml_vduse_blk_req *req, u64 sector,
				   blk_opf_t opf)
{
	struct block_device *bdev = req->blk->bdev;
	const struct iov_iter *it = &req->data;
	size_t left = iov_iter_count(it), off = it->iov_offset, done = 0;
	const struct kvec *kv = it->kvec;
	unsigned int nr_vecs;
	struct bio *bio;

	nr_vecs = min_t(size_t, DIV_ROUND_UP(left, PAGE_SIZE) + it->nr_segs,
			BIO_MAX_VECS);
	bio = bio_alloc(bdev, nr_vecs, opf, GFP_KERNEL);
	bio->bi_iter.bi_sector = sector;

	for (; left; kv++, off = 0) {
		size_t seg = min(kv->iov_len - off, left);
		void *p = kv->iov_base + off;

		left -= seg;
		while (seg) {
			unsigned int n = min_t(size_t, seg,
					       PAGE_SIZE - offset_in_page(p));
			struct page *page = virt_to_page(p);

			if (bio_add_page(bio, page, n, offset_in_page(p)) != n) {
				bio = blk_next_bio(bio, bdev, nr_vecs, opf,
						   GFP_KERNEL);
				bio->bi_iter.bi_sector = sector +
					(done >> SECTOR_SHIFT);
				__bio_add_page(bio, page, n, offset_in_page(p));
			}
			p += n;
			seg -= n;
			done += n;
		}
	}

	bio->bi_private = req;
	bio->bi_end_io = uml_vduse_blk_end_io;
	submit_bio(bio);
}

static int uml_vduse_blk_rw(struct uml_vduse_blk_req *req, u64 sector,
			    blk_opf_t opf)
{
	struct uml_vduse_blk *blk = req->blk;
	struct block_device *bdev = blk->bdev;
	size_t left = iov_iter_count(&req->data);
	u64 capacity = bdev_nr_sectors(bdev);
	unsigned int i, nr_vecs;
	struct bio *bio;
	int err;

	if (sector > capacity || (left >> SECTOR_SHIFT) > capacity - sector ||
	    !IS_ALIGNED(left, bdev_logical_block_size(bdev)) ||
	    !IS_ALIGNED(sector << SECTOR_SHIFT, bdev_logical_block_size(bdev)))
		return -EINVAL;

	if (!left) {
		uml_vduse_blk_finish(req, VIRTIO_BLK_S_OK, 1);
		return 0;
	}

	if (uml_vduse_blk_in_pages(req)) {
		uml_vduse_blk_rw_pages(req, sector, opf);
		return 0;
	}

	err = uml_vduse_blk_alloc_pages(req);
	if (err)
		return err;

	if (op_is_write(opf) && !uml_vduse_blk_copy(req, true))
		return -EFAULT;

	nr_vecs = min_t(unsigned int, req->nr_pages, BIO_MAX_VECS);
	bio = bio_alloc(bdev, nr_vecs, opf, GFP_KERNEL);
	bio->bi_iter.bi_sector = sector;

	for (i = 0; i < req->nr_pages; i++) {
		unsigned int n = min_t(size_t, left, PAGE_SIZE);

		if (bio_add_page(bio, req->pages[i], n, 0) != n) {
			nr_vecs = min_t(unsigned int, req->nr_pages - i,
					BIO_MAX_VECS);
			bio = blk_next_bio(bio, bdev, nr_vecs, opf, GFP_KERNEL);
			bio->bi_iter.bi_sector = sector +
				((size_t)i << (PAGE_SHIFT - SECTOR_SHIFT));
			__bio_add_page(bio, req->pages[i], n, 0);
		}
		left -= n;
	}

	bio->bi_private = req;
	bio->bi_end_io = uml_vduse_blk_end_io;
	submit_bio(bio);
	return 0;
}

static void uml_vduse_blk_flush(struct uml_vduse_blk_req *req)
{
	struct bio *bio;

	bio = bio_alloc(req->blk->bdev, 0, REQ_OP_WRITE | REQ_PREFLUSH,
			GFP_KERNEL);
	bio->bi_private = req;
	bio->bi_end_io = uml_vduse_blk_end_io;
	submit_bio(bio);
}

/* Return a pointer to the last writable byte, which holds the status. */
static u8 *uml_vduse_blk_status(struct vringh_kiov *wiov, size_t *len)
{
	u8 *status = NULL;
	size_t total = 0;
	unsigned int i;

	for (i = 0; i < wiov->used; i++) {
		if (wiov->iov[i].iov_len)
			status = wiov->iov[i].iov_base +
				 wiov->iov[i].iov_len - 1;
		total += wiov->iov[i].iov_len;
	}

	*len = total ? total - 1 : 0;
	return status;
}

static void uml_vduse_blk_handle(struct uml_vduse_blk_req *req)
{
	struct uml_vduse_blk *blk = req->blk;
	struct virtio_blk_outhdr hdr;
	struct iov_iter riter;
	size_t wlen, n;
	u64 sector;
	int err;

	req->status = uml_vduse_blk_status(&req->wiov, &wlen);
	if (!req->status) {
		uml_vduse_blk_finish(req, 0, 0);
		return;
	}

	iov_iter_kvec(&riter, ITER_SOURCE, req->riov.iov, req->riov.used,
		      vringh_kiov_length(&req->riov));
	if (copy_from_iter(&hdr, sizeof(hdr), &riter) != sizeof(hdr)) {
		uml_vduse_blk_finish(req, VIRTIO_BLK_S_IOERR, 1);
		return;
	}

	req->type = __virtio32_to_cpu(true, hdr.type);
	sector = __virtio64_to_cpu(true, hdr.sector);

	switch (req->type) {
	case VIRTIO_BLK_T_IN:
		iov_iter_kvec(&req->data, ITER_DEST, req->wiov.iov,
			      req->wiov.used, wlen);
		err = uml_vduse_blk_rw(req, sector, REQ_OP_READ);
		break;
	case VIRTIO_BLK_T_OUT:
		req->data = riter;
		err = uml_vduse_blk_rw(req, sector, REQ_OP_WRITE);
		break;
	case VIRTIO_BLK_T_FLUSH:
		uml_vduse_blk_flush(req);
		return;
	case VIRTIO_BLK_T_GET_ID:
		iov_iter_kvec(&req->data, ITER_DEST, req->wiov.iov,
			      req->wiov.used, wlen);
		n = copy_to_iter(blk->id, min(wlen, sizeof(blk->id)),
				 &req->data);
		uml_vduse_blk_finish(req, VIRTIO_BLK_S_OK, n + 1);
		return;
	default:
		uml_vduse_blk_finish(req, VIRTIO_BLK_S_UNSUPP, 1);
		return;
	}

	if (err)
		uml_vduse_blk_finish(req, VIRTIO_BLK_S_IOERR, 1);
}

static void uml_vduse_blk_work(struct work_struct *work)
{
	struct uml_vduse_blk *blk = container_of(to_delayed_work(work),
						 struct uml_vduse_blk, work);
	struct uml_vduse_blk_req *req;
	int r;

	do {
		uml_vduse_notify_disable(blk->dev, 0);
		for (;;) {
			/*
			 * Notifications stay disabled until the retry, which
			 * finds the requests that are still available.
			 */
			req = uml_vduse_blk_alloc(blk);
			if (!req) {
				schedule_delayed_work(&blk->work,
						      UML_VDUSE_BLK_RETRY_DELAY);
				return;
			}

			r = uml_vduse_getdesc(blk->dev, 0, &req->riov,
					      &req->wiov, &req->head);
			if (r <= 0) {
				uml_vduse_blk_free(req);
				if (r < 0)
					return;
				break;
			}

			atomic_inc(&blk->inflight);
			uml_vduse_blk_handle(req);
		}
	} while (!uml_vduse_notify_enable(blk->dev, 0));
}

static void uml_vduse_blk_kick(void *priv, unsigned int index)
{
	struct uml_vduse_blk *blk = priv;

	mod_delayed_work(system_percpu_wq, &blk->work, 0);
}

static int uml_vduse_blk_start(void *priv)
{
	return 0;
}

static void uml_vduse_blk_stop(void *priv)
{
	struct uml_vduse_blk *blk = priv;

	cancel_delayed_work_sync(&blk->work);
	wait_event(blk->idle, !atomic_read(&blk->inflight));
}

static void *__init uml_vduse_blk_setup(struct uml_vduse_dev *dev, char *opts,
					struct uml_vduse_config *cfg)
{
	const char *name = uml_vduse_name(dev);
	struct uml_vduse_blk *blk;
	char *disk = NULL, *opt;
	dev_t devt;
	int err;

	while ((opt = strsep(&opts, ","))) {
		if (strstarts(opt, "disk="))
			disk = opt + strlen("disk=");
		else if (*opt)
			return ERR_PTR(-EINVAL);
	}
	if (!disk)
		return ERR_PTR(-EINVAL);

	err = early_lookup_bdev(disk, &devt);
	if (err) {
		pr_err("%s: cannot find disk %s\n", name, disk);
		return ERR_PTR(err);
	}

	blk = kzalloc_obj(*blk);
	if (!blk)
		return ERR_PTR(-ENOMEM);

	blk->bdev_file = bdev_file_open_by_dev(devt,
					       BLK_OPEN_READ | BLK_OPEN_WRITE,
					       blk, NULL);
	if (IS_ERR(blk->bdev_file)) {
		err = PTR_ERR(blk->bdev_file);
		kfree(blk);
		return ERR_PTR(err);
	}
	blk->bdev = file_bdev(blk->bdev_file);

	blk->dev = dev;
	INIT_DELAYED_WORK(&blk->work, uml_vduse_blk_work);
	init_waitqueue_head(&blk->idle);
	memcpy(blk->id, name, min(strlen(name), sizeof(blk->id)));

	/*
	 * A request's chain has a descriptor for each segment, the header and
	 * the status byte, and must not be longer than the queue.
	 */
	blk->config.capacity = __cpu_to_virtio64(true,
						 bdev_nr_sectors(blk->bdev));
	blk->config.seg_max = __cpu_to_virtio32(true,
						UML_VDUSE_BLK_QUEUE_SIZE - 2);
	blk->config.blk_size =
		__cpu_to_virtio32(true, bdev_logical_block_size(blk->bdev));

	cfg->device_id = VIRTIO_ID_BLOCK;
	cfg->features = BIT_ULL(VIRTIO_BLK_F_SEG_MAX) |
			BIT_ULL(VIRTIO_BLK_F_BLK_SIZE) |
			BIT_ULL(VIRTIO_BLK_F_FLUSH);
	cfg->num_queues = 1;
	cfg->queue_size = UML_VDUSE_BLK_QUEUE_SIZE;
	cfg->config = &blk->config;
	cfg->config_size = sizeof(blk->config);

	pr_info("%s: serving disk %s with %llu sectors\n", name, disk,
		(unsigned long long)bdev_nr_sectors(blk->bdev));
	return blk;
}

static void uml_vduse_blk_release(void *priv)
{
	struct uml_vduse_blk *blk = priv;

	fput(blk->bdev_file);
	kfree(blk);
}

const struct uml_vduse_backend_ops uml_vduse_blk_ops = {
	.type = "blk",
	.setup = uml_vduse_blk_setup,
	.release = uml_vduse_blk_release,
	.start = uml_vduse_blk_start,
	.stop = uml_vduse_blk_stop,
	.kick = uml_vduse_blk_kick,
};
