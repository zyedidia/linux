/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_ARM64_PTRACE_H
#define __SYSDEP_ARM64_PTRACE_H

#include <generated/user_constants.h>
#include <sysdep/faultinfo.h>

/*
 * The gp array mirrors the host's struct user_regs_struct: x0-x30 at their
 * register number, then sp, pc, pstate.  HOST_* index constants come from
 * user-offsets.c.
 */
#define MAX_REG_OFFSET (UM_FRAME_SIZE)
#define MAX_REG_NR ((MAX_REG_OFFSET) / sizeof(unsigned long))

#define REGS_X(r, n) ((r)[n])
#define REGS_IP(r) ((r)[HOST_IP])
#define REGS_SP(r) ((r)[HOST_SP])
#define REGS_PSTATE(r) ((r)[HOST_PSTATE])

#define UPT_X(r, n) REGS_X((r)->gp, n)
#define UPT_IP(r) REGS_IP((r)->gp)
#define UPT_SP(r) REGS_SP((r)->gp)
#define UPT_PSTATE(r) REGS_PSTATE((r)->gp)
#define UPT_FP(r) UPT_X(r, 29)
#define UPT_LR(r) UPT_X(r, 30)

/* AAPCS64 syscall convention: number in x8, arguments in x0-x5. */
#define UPT_SYSCALL_ARG1(r) UPT_X(r, 0)
#define UPT_SYSCALL_ARG2(r) UPT_X(r, 1)
#define UPT_SYSCALL_ARG3(r) UPT_X(r, 2)
#define UPT_SYSCALL_ARG4(r) UPT_X(r, 3)
#define UPT_SYSCALL_ARG5(r) UPT_X(r, 4)
#define UPT_SYSCALL_ARG6(r) UPT_X(r, 5)
#define UPT_SYSCALL_RET(r) UPT_X(r, 0)

extern unsigned long host_fp_size;

struct uml_pt_regs {
	unsigned long gp[MAX_REG_NR];
	struct faultinfo faultinfo;
	long syscall;
	int is_user;

	/* Dynamically sized FP register state */
	unsigned long fp[];
};

#define EMPTY_UML_PT_REGS { }

#define UPT_SYSCALL_NR(r) ((r)->syscall)
#define UPT_FAULTINFO(r) (&(r)->faultinfo)
#define UPT_IS_USER(r) ((r)->is_user)

/*
 * Rewinds the PC over the svc instruction so the syscall is replayed, the
 * same thing REGS_RESTART_SYSCALL does on x86 with the int80/syscall insn.
 * Only a task returning from a syscall can get here, which takes userspace
 * support, so this is never reached in this kernel-only port.
 */
#define UPT_RESTART_SYSCALL(r) (UPT_IP(r) -= 4)

extern int arch_init_registers(int pid);

#endif /* __SYSDEP_ARM64_PTRACE_H */
