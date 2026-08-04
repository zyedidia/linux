// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

/*
 * Kernel side of UML's bring-up and thread switching.  Split out of
 * skas/process.c because none of it needs a userspace process; it is built
 * even when CONFIG_UML_NO_USERSPACE is set.
 */

#include <linux/init.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/smp-internal.h>
#include <linux/spinlock.h>

#include <kern_util.h>
#include <os.h>
#include <skas.h>

extern void start_kernel(void);

static int __init start_kernel_proc(void *unused)
{
	block_signals_trace();

	start_kernel();
	return 0;
}

char cpu_irqstacks[NR_CPUS][THREAD_SIZE] __aligned(THREAD_SIZE);

int __init start_uml(void)
{
	stack_protections((unsigned long) &cpu_irqstacks[0]);
	set_sigstack(cpu_irqstacks[0], THREAD_SIZE);

	init_new_thread_signals();

	init_task.thread.request.thread.proc = start_kernel_proc;
	init_task.thread.request.thread.arg = NULL;
	return start_idle_thread(task_stack_page(&init_task),
				 &init_task.thread.switch_buf);
}

static DEFINE_SPINLOCK(initial_jmpbuf_spinlock);

void initial_jmpbuf_lock(void)
{
	spin_lock_irq(&initial_jmpbuf_spinlock);
}

void initial_jmpbuf_unlock(void)
{
	spin_unlock_irq(&initial_jmpbuf_spinlock);
}
