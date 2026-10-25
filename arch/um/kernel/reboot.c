// SPDX-License-Identifier: GPL-2.0
/* 
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/sched/mm.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/oom.h>
#include <linux/reboot.h>
#include <linux/eventfd.h>
#include <linux/interrupt.h>
#include <linux/suspend.h>
#include <linux/syscalls.h>
#include <linux/workqueue.h>
#include <irq_kern.h>
#include <kern_util.h>
#include <os.h>
#include <skas.h>

void (*pm_power_off)(void);
EXPORT_SYMBOL(pm_power_off);

static void kill_off_processes(void)
{
	struct task_struct *p;
	int pid;

	read_lock(&tasklist_lock);
	for_each_process(p) {
		struct task_struct *t;

		t = find_lock_task_mm(p);
		if (!t)
			continue;
		pid = t->mm->context.id.pid;
		task_unlock(t);
		os_kill_ptraced_process(pid, 1);
	}
	read_unlock(&tasklist_lock);
}

void uml_cleanup(void)
{
	kmalloc_ok = 0;
	do_uml_exitcalls();
	kill_off_processes();
}

void machine_restart(char * __unused)
{
	uml_cleanup();
	reboot_skas();
}

void machine_power_off(void)
{
	uml_cleanup();
	halt_skas();
}

void machine_halt(void)
{
	machine_power_off();
}

static int sys_power_off_handler(struct sys_off_data *data)
{
	machine_power_off();
	return 0;
}

static int register_power_off(void)
{
	register_sys_off_handler(SYS_OFF_MODE_POWER_OFF,
				 SYS_OFF_PRIO_DEFAULT,
				 sys_power_off_handler, NULL);
	return 0;
}
__initcall(register_power_off);

static int uml_shutdown_fd = -1;

/*
 * Power off in order, so that drivers run their shutdown callbacks and
 * release their devices. This does not depend on guest userspace.
 */
static void uml_shutdown_work_fn(struct work_struct *work)
{
	pr_info("Powering off after a host SIGINT or SIGTERM\n");
	ksys_sync();
	mutex_lock(&system_transition_mutex);
	kernel_power_off();
}
static DECLARE_WORK(uml_shutdown_work, uml_shutdown_work_fn);

static irqreturn_t uml_shutdown_interrupt(int irq, void *data)
{
	u64 v;

	if (os_read_file(uml_shutdown_fd, &v, sizeof(v)) == sizeof(v))
		schedule_work_on(cpumask_first(cpu_online_mask),
				 &uml_shutdown_work);
	return IRQ_HANDLED;
}

/*
 * Until this runs, SIGINT and SIGTERM exit at once. Afterwards the signal
 * handler writes to an eventfd, which raises an interrupt here.
 */
static int __init uml_shutdown_init(void)
{
	int fd, irq, err;

	fd = os_eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fd < 0)
		return fd;

	irq = um_request_irq(UM_IRQ_ALLOC, fd, IRQ_READ, uml_shutdown_interrupt,
			     0, "shutdown", NULL);
	if (irq < 0) {
		err = irq;
		goto close;
	}

	/* Eventfds cannot raise SIGIO, so the SIGIO helper watches it. */
	sigio_broken();
	err = add_sigio_fd(fd);
	if (err)
		goto free_irq;

	uml_shutdown_fd = fd;
	os_set_shutdown_fd(fd);
	return 0;

free_irq:
	um_free_irq(irq, NULL);
close:
	os_close_file(fd);
	pr_warn("SIGINT and SIGTERM will not power off in order, error %d\n",
		err);
	return err;
}
late_initcall(uml_shutdown_init);
