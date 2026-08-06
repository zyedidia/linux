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

/*
 * Whether uml_cleanup() may run in the current context. The exitcalls
 * can block - closing a console channel ends in free_irq() ->
 * synchronize_rcu() - and a fatal signal usually interrupts the idle
 * task, which must not sleep: on an SMP kernel the exit then wedges in
 * a schedule-from-idle loop. Tiny RCU never blocks there, which is
 * what kept the UP exit path working all along.
 */
int uml_cleanup_safe(void)
{
	return IS_ENABLED(CONFIG_TINY_RCU) || !is_idle_task(current);
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
