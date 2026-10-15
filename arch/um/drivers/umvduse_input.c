// SPDX-License-Identifier: GPL-2.0
/*
 * umvduse-input: export an input device driven inside this UML instance
 * (e.g. a USB mouse behind a passed-through xHCI controller) to the host
 * as a virtio-input device.
 *
 * An input handler binds to the first input device that appears, or the
 * first whose name contains umvduse_input.name=, and forwards its events
 * into the eventq buffers the host driver posts. The host's virtio_input
 * driver learns the device's identity and capabilities by writing a
 * (select, subsel) pair into config space and reading the answer back;
 * each write reaches us through the transport's config_write hook and
 * is answered from the bound device on the spot. statusq carries LED
 * (and similar) changes from the host, which are injected into the
 * device.
 *
 * The host describes the device once, when its driver probes at
 * `vdpa dev add`, so attach it after the input device has shown up
 * here. A device bound later is not described to an already-probed
 * host driver; vdpa dev del/add picks it up.
 */

#define pr_fmt(fmt) "umvduse-input: " fmt

#include <linux/input.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <uapi/linux/virtio_ids.h>
#include <uapi/linux/virtio_input.h>

#include <init.h>

#include "umvduse.h"

#define UMVD_INPUT_EVENTQ	0
#define UMVD_INPUT_STATUSQ	1
#define UMVD_INPUT_QUEUE_SIZE	64
/* Events waiting for the host to post more buffers. */
#define UMVD_INPUT_BACKLOG	128

struct umvd_input_buf {
	struct list_head list;
	struct umvd_request *req;
};

struct umvd_input {
	/* Binding of the input device vs. config answers and statusq. */
	struct mutex lock;
	struct input_handle *handle;

	/*
	 * eventq buffers and the backlog. Events are written and their
	 * buffers completed under this lock, so they reach the host in
	 * order whichever context (input event, buffer arrival) moves
	 * them. Completing takes the transport's queue lock and makes a
	 * host syscall; neither calls back into this shim.
	 */
	spinlock_t ev_lock;
	struct list_head bufs;
	bool running;
	struct virtio_input_event backlog[UMVD_INPUT_BACKLOG];
	unsigned int backlog_head, backlog_count;
	u64 dropped;
};

static struct umvd_input umvd_input;

static char umvd_input_match[64];
module_param_string(name, umvd_input_match, sizeof(umvd_input_match), 0400);
__uml_help(umvd_input_match,
"umvduse_input.name=<substring>\n"
"    Export the first input device whose name contains <substring>\n"
"    instead of the first input device to appear.\n\n"
);

static size_t kiov_to_buf(struct vringh_kiov *kiov, void *buf, size_t len)
{
	size_t off = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used && off < len; i++) {
		size_t c = min_t(size_t, kiov->iov[i].iov_len, len - off);

		memcpy((u8 *)buf + off, kiov->iov[i].iov_base, c);
		off += c;
	}
	return off;
}

static size_t kiov_from_buf(struct vringh_kiov *kiov, const void *buf,
			    size_t len)
{
	size_t off = 0;
	unsigned int i;

	for (i = kiov->i; i < kiov->used && off < len; i++) {
		size_t c = min_t(size_t, kiov->iov[i].iov_len, len - off);

		memcpy(kiov->iov[i].iov_base, (const u8 *)buf + off, c);
		off += c;
	}
	return off;
}

/* ---- events -------------------------------------------------------- */

/* Pair backlogged events with posted buffers. Called with ev_lock held. */
static void umvd_input_flush_locked(struct umvd_input *in)
{
	while (in->backlog_count && !list_empty(&in->bufs)) {
		struct virtio_input_event *ev = &in->backlog[in->backlog_head];
		struct umvd_input_buf *buf;
		size_t n;

		buf = list_first_entry(&in->bufs, struct umvd_input_buf, list);
		list_del(&buf->list);

		n = kiov_from_buf(&buf->req->wiov, ev, sizeof(*ev));
		umvd_request_complete(buf->req, n);
		kfree(buf);

		in->backlog_head = (in->backlog_head + 1) % UMVD_INPUT_BACKLOG;
		in->backlog_count--;
	}
}

static void umvd_input_event(struct input_handle *handle, unsigned int type,
			     unsigned int code, int value)
{
	struct umvd_input *in = handle->private;
	struct virtio_input_event *ev;
	unsigned int tail;

	/*
	 * The host repeats keys itself when EV_REP is advertised, and LED
	 * and sound events only ever arrive here because the host sent
	 * them down statusq.
	 */
	if ((type == EV_KEY && value == 2) || type == EV_LED || type == EV_SND)
		return;

	guard(spinlock_irqsave)(&in->ev_lock);

	/* Nothing to deliver to until the host driver is up. */
	if (!in->running)
		return;

	if (in->backlog_count == UMVD_INPUT_BACKLOG) {
		/* Keep the newest: drop the oldest event. */
		in->backlog_head = (in->backlog_head + 1) % UMVD_INPUT_BACKLOG;
		in->backlog_count--;
		if (!(in->dropped++ % 1000))
			pr_warn_ratelimited("host is not taking events, dropped %llu\n",
					    in->dropped);
	}

	tail = (in->backlog_head + in->backlog_count) % UMVD_INPUT_BACKLOG;
	ev = &in->backlog[tail];
	ev->type = cpu_to_le16(type);
	ev->code = cpu_to_le16(code);
	ev->value = cpu_to_le32(value);
	in->backlog_count++;

	umvd_input_flush_locked(in);
}

static void umvd_input_park(struct umvd_input *in, struct umvd_request *req)
{
	struct umvd_input_buf *buf;

	buf = kmalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf) {
		umvd_request_complete(req, 0);
		return;
	}
	buf->req = req;

	scoped_guard(spinlock_irqsave, &in->ev_lock) {
		if (in->running) {
			list_add_tail(&buf->list, &in->bufs);
			umvd_input_flush_locked(in);
			return;
		}
	}

	kfree(buf);
	umvd_request_complete(req, 0);
}

/* The host changed an LED (or similar): pass it to the device. */
static void umvd_input_status(struct umvd_input *in, struct umvd_request *req)
{
	struct virtio_input_event ev;

	if (kiov_to_buf(&req->riov, &ev, sizeof(ev)) == sizeof(ev)) {
		guard(mutex)(&in->lock);
		if (in->handle)
			input_inject_event(in->handle, le16_to_cpu(ev.type),
					   le16_to_cpu(ev.code),
					   (s32)le32_to_cpu(ev.value));
	}
	umvd_request_complete(req, 0);
}

/* ---- config space -------------------------------------------------- */

/* Little-endian byte bitmap, as virtio-input wants; returns its length. */
static u8 umvd_input_bitmap(u8 *out, const unsigned long *bits,
			    unsigned int nbits)
{
	unsigned int bit;
	u8 len = 0;

	nbits = min_t(unsigned int, nbits,
		      sizeof_field(struct virtio_input_config, u.bitmap) * 8);
	for_each_set_bit(bit, bits, nbits) {
		out[bit / 8] |= BIT(bit % 8);
		len = bit / 8 + 1;
	}
	return len;
}

static u8 umvd_input_ev_bits(struct input_dev *idev, u8 type, u8 *out)
{
	if (!test_bit(type, idev->evbit))
		return 0;

	switch (type) {
	case EV_KEY:
		return umvd_input_bitmap(out, idev->keybit, KEY_CNT);
	case EV_REL:
		return umvd_input_bitmap(out, idev->relbit, REL_CNT);
	case EV_ABS:
		return umvd_input_bitmap(out, idev->absbit, ABS_CNT);
	case EV_MSC:
		return umvd_input_bitmap(out, idev->mscbit, MSC_CNT);
	case EV_SW:
		return umvd_input_bitmap(out, idev->swbit, SW_CNT);
	case EV_LED:
		return umvd_input_bitmap(out, idev->ledbit, LED_CNT);
	case EV_SND:
		return umvd_input_bitmap(out, idev->sndbit, SND_CNT);
	case EV_REP:
		/* Only presence matters; the host runs its own repeat. */
		return 1;
	default:
		return 0;
	}
}

static int umvd_input_config_write(struct umvd_dev *dev, u32 offset, u32 len)
{
	struct umvd_input *in = dev->priv;
	struct virtio_input_config *cfg = (void *)dev->config;
	struct input_dev *idev;
	const char *str;
	u8 size = 0;

	/* Everything after select/subsel is the answer to them. */
	memset(&cfg->size, 0, sizeof(*cfg) - offsetof(typeof(*cfg), size));

	guard(mutex)(&in->lock);

	if (!in->handle)
		return 0;
	idev = in->handle->dev;

	switch (cfg->select) {
	case VIRTIO_INPUT_CFG_ID_NAME:
	case VIRTIO_INPUT_CFG_ID_SERIAL:
		str = cfg->select == VIRTIO_INPUT_CFG_ID_NAME ? idev->name :
								idev->uniq;
		if (str && !cfg->subsel) {
			size = strnlen(str, sizeof(cfg->u.string));
			memcpy(cfg->u.string, str, size);
		}
		break;
	case VIRTIO_INPUT_CFG_ID_DEVIDS:
		if (!cfg->subsel) {
			cfg->u.ids.bustype = cpu_to_le16(idev->id.bustype);
			cfg->u.ids.vendor = cpu_to_le16(idev->id.vendor);
			cfg->u.ids.product = cpu_to_le16(idev->id.product);
			cfg->u.ids.version = cpu_to_le16(idev->id.version);
			size = sizeof(cfg->u.ids);
		}
		break;
	case VIRTIO_INPUT_CFG_PROP_BITS:
		if (!cfg->subsel)
			size = umvd_input_bitmap(cfg->u.bitmap, idev->propbit,
						 INPUT_PROP_CNT);
		break;
	case VIRTIO_INPUT_CFG_EV_BITS:
		size = umvd_input_ev_bits(idev, cfg->subsel, cfg->u.bitmap);
		break;
	case VIRTIO_INPUT_CFG_ABS_INFO:
		if (cfg->subsel < ABS_CNT && test_bit(cfg->subsel, idev->absbit) &&
		    idev->absinfo) {
			const struct input_absinfo *abs = &idev->absinfo[cfg->subsel];

			cfg->u.abs.min = cpu_to_le32(abs->minimum);
			cfg->u.abs.max = cpu_to_le32(abs->maximum);
			cfg->u.abs.fuzz = cpu_to_le32(abs->fuzz);
			cfg->u.abs.flat = cpu_to_le32(abs->flat);
			cfg->u.abs.res = cpu_to_le32(abs->resolution);
			size = sizeof(cfg->u.abs);
		}
		break;
	}

	cfg->size = size;
	return 0;
}

/* ---- input handler ------------------------------------------------- */

static int umvd_input_connect(struct input_handler *handler,
			      struct input_dev *idev,
			      const struct input_device_id *id)
{
	struct umvd_input *in = &umvd_input;
	struct input_handle *handle;
	int err;

	if (umvd_input_match[0] &&
	    (!idev->name || !strstr(idev->name, umvd_input_match)))
		return -ENODEV;

	guard(mutex)(&in->lock);

	/* One VDUSE device, one input device. */
	if (in->handle)
		return -EBUSY;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = idev;
	handle->handler = handler;
	handle->name = "umvduse-input";
	handle->private = in;

	err = input_register_handle(handle);
	if (err)
		goto free;

	/* Opening is what starts the device reporting (e.g. usbhid's URBs). */
	err = input_open_device(handle);
	if (err)
		goto unregister;

	in->handle = handle;
	pr_info("exporting \"%s\"\n", idev->name ?: "?");
	return 0;

unregister:
	input_unregister_handle(handle);
free:
	kfree(handle);
	return err;
}

static void umvd_input_disconnect(struct input_handle *handle)
{
	struct umvd_input *in = handle->private;

	scoped_guard(mutex, &in->lock)
		in->handle = NULL;

	pr_info("\"%s\" went away\n", handle->dev->name ?: "?");
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id umvd_input_ids[] = {
	{
		/* Every device: they all have EV_SYN. */
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_SYN) },
	},
	{ },
};

static struct input_handler umvd_input_handler = {
	.name		= "umvduse-input",
	.event		= umvd_input_event,
	.connect	= umvd_input_connect,
	.disconnect	= umvd_input_disconnect,
	.id_table	= umvd_input_ids,
};

/* ---- umvduse shim -------------------------------------------------- */

static int umvd_input_setup(struct umvd_dev *dev)
{
	dev->device_id = VIRTIO_ID_INPUT;
	dev->num_queues = 2;
	dev->queue_max[UMVD_INPUT_EVENTQ] = UMVD_INPUT_QUEUE_SIZE;
	dev->queue_max[UMVD_INPUT_STATUSQ] = UMVD_INPUT_QUEUE_SIZE;
	dev->config_size = sizeof(struct virtio_input_config);
	return 0;
}

static int umvd_input_start(struct umvd_dev *dev, u64 features)
{
	struct umvd_input *in = dev->priv;

	scoped_guard(spinlock_irqsave, &in->ev_lock)
		in->running = true;
	return 0;
}

static void umvd_input_handle(struct umvd_dev *dev, struct umvd_request *req)
{
	struct umvd_input *in = dev->priv;

	switch (umvd_request_queue(req)) {
	case UMVD_INPUT_EVENTQ:
		umvd_input_park(in, req);
		break;
	case UMVD_INPUT_STATUSQ:
		umvd_input_status(in, req);
		break;
	default:
		umvd_request_complete(req, 0);
		break;
	}
}

static void umvd_input_reset(struct umvd_dev *dev)
{
	struct umvd_input *in = dev->priv;
	struct umvd_input_buf *buf, *n;
	LIST_HEAD(bufs);

	scoped_guard(spinlock_irqsave, &in->ev_lock) {
		in->running = false;
		list_splice_init(&in->bufs, &bufs);
		in->backlog_count = 0;
	}

	/* Hand every parked buffer back; the transport discards them. */
	list_for_each_entry_safe(buf, n, &bufs, list) {
		list_del(&buf->list);
		umvd_request_complete(buf->req, 0);
		kfree(buf);
	}
}

static const struct umvd_device_ops umvd_input_ops = {
	.name		= "input",
	.setup		= umvd_input_setup,
	.start		= umvd_input_start,
	.handle		= umvd_input_handle,
	.reset		= umvd_input_reset,
	.config_write	= umvd_input_config_write,
};

static int __init umvd_input_init(void)
{
	struct umvd_input *in = &umvd_input;
	int err;

	mutex_init(&in->lock);
	spin_lock_init(&in->ev_lock);
	INIT_LIST_HEAD(&in->bufs);

	err = input_register_handler(&umvd_input_handler);
	if (err)
		return err;

	err = umvd_register_device(&umvd_input_ops, in);
	if (err)
		input_unregister_handler(&umvd_input_handler);
	return err;
}
late_initcall(umvd_input_init);
