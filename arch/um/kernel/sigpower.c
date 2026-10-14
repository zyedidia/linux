// SPDX-License-Identifier: GPL-2.0
/*
 * Orderly shutdown on SIGTERM/SIGINT for a kernel-only UML.
 *
 * A driver domain owns real hardware, and killing the process abandons
 * that hardware mid-operation: a VFIO-passed WiFi card is left with its
 * firmware running and needs a bus reset before it will probe again.
 * Turning the host's signal into kernel_power_off() runs every driver's
 * .shutdown callback first, so the device comes back cleanly.
 *
 * The signal handler (os side) writes a byte into a pipe whose read end
 * is registered as an IRQ here; the IRQ schedules the power-off from
 * process context. The first signal takes this path; a second one kills
 * the process the old way (SA_RESETHAND in the handler).
 */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/reboot.h>
#include <linux/workqueue.h>
#include <irq_kern.h>
#include <os.h>

static int sigpower_fd = -1;

static void sigpower_work_fn(struct work_struct *work)
{
	pr_info("sigpower: powering off on host signal\n");
	kernel_power_off();
}
static DECLARE_WORK(sigpower_work, sigpower_work_fn);

static irqreturn_t sigpower_interrupt(int irq, void *dev)
{
	char c;

	while (os_read_file(sigpower_fd, &c, sizeof(c)) == sizeof(c))
		;
	schedule_work(&sigpower_work);
	return IRQ_HANDLED;
}

static int __init sigpower_init(void)
{
	int fds[2], irq, err;

	err = os_pipe(fds, 1, 1);
	if (err < 0)
		return err;

	os_set_fd_block(fds[0], 0);
	os_set_fd_block(fds[1], 0);
	sigpower_fd = fds[0];

	irq = um_request_irq(UM_IRQ_ALLOC, fds[0], IRQ_READ,
			     sigpower_interrupt, 0, "poweroff", NULL);
	if (irq < 0) {
		err = irq;
		goto out_close;
	}

	err = add_sigio_fd(fds[0]);
	if (err)
		goto out_irq;

	os_arm_graceful_exit(fds[1]);
	return 0;

out_irq:
	um_free_irq(irq, NULL);
out_close:
	sigpower_fd = -1;
	os_close_file(fds[0]);
	os_close_file(fds[1]);
	return err;
}
late_initcall(sigpower_init);
