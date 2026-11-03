/* SPDX-License-Identifier: GPL-2.0 */
/*
 * These wrappers give every UML/x86-64 system call the type
 * long (const struct pt_regs *), so that um_sys_call() calls each one through
 * its real prototype. They are based on
 * arch/arm64/include/asm/syscall_wrapper.h.
 */

#ifndef __UM_ASM_SYSCALL_WRAPPER_H
#define __UM_ASM_SYSCALL_WRAPPER_H

#include <asm/ptrace.h>

#define SC_UM_REGS_TO_ARGS(x, ...)					\
	__MAP(x,__SC_ARGS						\
	      ,,UPT_SYSCALL_ARG1(&regs->regs)				\
	      ,,UPT_SYSCALL_ARG2(&regs->regs)				\
	      ,,UPT_SYSCALL_ARG3(&regs->regs)				\
	      ,,UPT_SYSCALL_ARG4(&regs->regs)				\
	      ,,UPT_SYSCALL_ARG5(&regs->regs)				\
	      ,,UPT_SYSCALL_ARG6(&regs->regs))

#define __SYSCALL_DEFINEx(x, name, ...)					\
	asmlinkage long __um_sys##name(const struct pt_regs *regs);	\
	ALLOW_ERROR_INJECTION(__um_sys##name, ERRNO);			\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__));	\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__));\
	asmlinkage long __um_sys##name(const struct pt_regs *regs)	\
	{								\
		return __se_sys##name(SC_UM_REGS_TO_ARGS(x,__VA_ARGS__));\
	}								\
	static long __se_sys##name(__MAP(x,__SC_LONG,__VA_ARGS__))	\
	{								\
		long ret = __do_sys##name(__MAP(x,__SC_CAST,__VA_ARGS__));\
		__MAP(x,__SC_TEST,__VA_ARGS__);				\
		__PROTECT(x, ret,__MAP(x,__SC_ARGS,__VA_ARGS__));	\
		return ret;						\
	}								\
	static inline long __do_sys##name(__MAP(x,__SC_DECL,__VA_ARGS__))

#define SYSCALL_DEFINE0(sname)						\
	SYSCALL_METADATA(_##sname, 0);					\
	asmlinkage long __um_sys_##sname(const struct pt_regs *__unused);\
	ALLOW_ERROR_INJECTION(__um_sys_##sname, ERRNO);			\
	asmlinkage long __um_sys_##sname(const struct pt_regs *__unused)

#define COND_SYSCALL(name)						\
	asmlinkage long __um_sys_##name(const struct pt_regs *regs);	\
	asmlinkage long __weak __um_sys_##name(const struct pt_regs *regs)\
	{								\
		return sys_ni_syscall();				\
	}

asmlinkage long __um_sys_ni_syscall(const struct pt_regs *__unused);

#endif /* __UM_ASM_SYSCALL_WRAPPER_H */
