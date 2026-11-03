/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ASM_SYSCALL_H
#define __UM_ASM_SYSCALL_H

#include <asm/syscall-generic.h>
#include <uapi/linux/audit.h>

#ifdef CONFIG_ARCH_HAS_SYSCALL_WRAPPER
#include <linux/string.h>

typedef long (*sys_call_ptr_t)(const struct pt_regs *);

extern const sys_call_ptr_t sys_call_table[];

long um_sys_call(const struct pt_regs *regs, unsigned int nr);

/*
 * The system call symbols carry a __um_ prefix, which the tracing code has to
 * skip when it looks up the metadata of a system call.
 */
#define ARCH_HAS_SYSCALL_MATCH_SYM_NAME
static inline bool arch_syscall_match_sym_name(const char *sym,
					       const char *name)
{
	return !strcmp(sym + 5, name);
}
#else
typedef asmlinkage long (*sys_call_ptr_t)(unsigned long, unsigned long,
					  unsigned long, unsigned long,
					  unsigned long, unsigned long);

extern const sys_call_ptr_t sys_call_table[];

static inline long um_sys_call(const struct pt_regs *regs, unsigned int nr)
{
	return sys_call_table[nr](UPT_SYSCALL_ARG1(&regs->regs),
				  UPT_SYSCALL_ARG2(&regs->regs),
				  UPT_SYSCALL_ARG3(&regs->regs),
				  UPT_SYSCALL_ARG4(&regs->regs),
				  UPT_SYSCALL_ARG5(&regs->regs),
				  UPT_SYSCALL_ARG6(&regs->regs));
}
#endif

static inline int syscall_get_arch(struct task_struct *task)
{
#ifdef CONFIG_X86_32
	return AUDIT_ARCH_I386;
#else
	return AUDIT_ARCH_X86_64;
#endif
}

#endif /* __UM_ASM_SYSCALL_H */
