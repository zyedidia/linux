/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_PTRACE_H
#define __UM_ARM64_PTRACE_H

#include <linux/compiler.h>

enum {
	REGSET_GENERAL,
};

#include <asm/ptrace-generic.h>

#define user_mode(r) UPT_IS_USER(&(r)->regs)

#define PT_REGS_X(r, n) UPT_X(&(r)->regs, n)
#define PT_REGS_SP(r) UPT_SP(&(r)->regs)
#define PT_REGS_FP(r) UPT_FP(&(r)->regs)
#define PT_REGS_BP(r) UPT_FP(&(r)->regs)
#define PT_REGS_LR(r) UPT_LR(&(r)->regs)
#define PT_REGS_PSTATE(r) UPT_PSTATE(&(r)->regs)

#define PT_REGS_ORIG_SYSCALL(r) PT_REGS_X(r, 8)
#define PT_REGS_SYSCALL_RET(r) PT_REGS_X(r, 0)
#define PT_REGS_SET_SYSCALL_RETURN(r, res) (PT_REGS_SYSCALL_RET(r) = (res))

#define profile_pc(regs) PT_REGS_IP(regs)
#define user_stack_pointer(regs) PT_REGS_SP(regs)

static inline long regs_return_value(struct pt_regs *regs)
{
	return PT_REGS_X(regs, 0);
}

struct task_struct;
extern void arch_switch_to(struct task_struct *to);

#endif /* __UM_ARM64_PTRACE_H */
