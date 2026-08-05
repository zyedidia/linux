// SPDX-License-Identifier: GPL-2.0
/*
 * Register dump for AArch64 UML.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/utsname.h>
#include <asm/current.h>
#include <asm/ptrace.h>

void show_regs(struct pt_regs *regs)
{
	int i;

	printk("\n");
	print_modules();
	printk(KERN_INFO "Pid: %d, comm: %.20s %s %s\n", task_pid_nr(current),
	       current->comm, print_tainted(), init_utsname()->release);
	printk(KERN_INFO "PC:  %pS\n", (void *)PT_REGS_IP(regs));
	printk(KERN_INFO "LR:  %pS\n", (void *)PT_REGS_LR(regs));
	printk(KERN_INFO "SP:  %016lx  PSTATE: %08lx\n", PT_REGS_SP(regs),
	       PT_REGS_PSTATE(regs));

	for (i = 28; i >= 0; i -= 3) {
		if (i >= 2)
			printk(KERN_INFO "x%-2d: %016lx x%-2d: %016lx x%-2d: %016lx\n",
			       i, PT_REGS_X(regs, i),
			       i - 1, PT_REGS_X(regs, i - 1),
			       i - 2, PT_REGS_X(regs, i - 2));
		else
			printk(KERN_INFO "x%-2d: %016lx x%-2d: %016lx x29: %016lx\n",
			       i, PT_REGS_X(regs, i),
			       i - 1, PT_REGS_X(regs, i - 1),
			       PT_REGS_FP(regs));
	}
}
