// SPDX-License-Identifier: GPL-2.0
/*
 * This file holds the system call table and dispatcher for UML/x86-64, based
 * on arch/x86/entry/syscall_64.c.
 */

#include <linux/linkage.h>
#include <linux/sys.h>
#include <linux/syscalls.h>
#include <asm/syscall.h>

asmlinkage long sys_ni_syscall(void);

asmlinkage long __um_sys_ni_syscall(const struct pt_regs *__unused)
{
	return sys_ni_syscall();
}

/*
 * Below you can see, in terms of #define's, the differences between the x86-64
 * and the UML syscall table.
 */

/* Not going to be implemented by UML, since we have no hardware. */
#define __um_sys_iopl __um_sys_ni_syscall
#define __um_sys_ioperm __um_sys_ni_syscall

#define __SYSCALL_NORETURN __SYSCALL

#define __SYSCALL(nr, sym) extern long __um_##sym(const struct pt_regs *);
#include <asm/syscalls_64.h>
#undef  __SYSCALL

/*
 * um_sys_call() dispatches the system calls, and the tracing code looks up
 * their addresses in sys_call_table[].
 */
#define __SYSCALL(nr, sym) __um_##sym,
const sys_call_ptr_t sys_call_table[] = {
#include <asm/syscalls_64.h>
};
#undef  __SYSCALL

int syscall_table_size = sizeof(sys_call_table);

#define __SYSCALL(nr, sym) case nr: return __um_##sym(regs);
long um_sys_call(const struct pt_regs *regs, unsigned int nr)
{
	switch (nr) {
	#include <asm/syscalls_64.h>
	default: return __um_sys_ni_syscall(regs);
	}
}
