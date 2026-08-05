// SPDX-License-Identifier: GPL-2.0
/*
 * Host register access for AArch64 UML.  Without userspace support nothing
 * ever ptraces a child or carries FP state, so the FP hooks stay inert;
 * host_fp_size = 0 keeps the shared code from touching FP buffers.
 */

#include <errno.h>
#include <longjmp.h>
#include <sysdep/ptrace_user.h>
#include <registers.h>

unsigned long host_fp_size;

int get_fp_registers(int pid, unsigned long *regs)
{
	return -EINVAL;
}

int put_fp_registers(int pid, unsigned long *regs)
{
	return -EINVAL;
}

int arch_init_registers(int pid)
{
	return 0;
}

unsigned long get_thread_reg(int reg, jmp_buf *buf)
{
	switch (reg) {
	case HOST_IP:
		return buf[0]->__pc;
	case HOST_SP:
		return buf[0]->__sp;
	case HOST_FP:
		return buf[0]->__fp;
	default:
		printk(UM_KERN_ERR "get_thread_regs - unknown register %d\n",
		       reg);
		return 0;
	}
}
