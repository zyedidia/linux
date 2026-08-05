/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ELF_ARM64_H
#define __UM_ELF_ARM64_H

#include <linux/types.h>

/*
 * Just enough ELF knowledge for the generic headers to compile.  Nothing can
 * exec a binary in this kernel-only port, so the process-ABI details
 * (ELF_PLAT_INIT and friends) are only ever referenced from dead code.
 */

#define ELF_CLASS ELFCLASS64
#define ELF_DATA ELFDATA2LSB
#define ELF_ARCH EM_AARCH64

#define elf_check_arch(x) ((x)->e_machine == EM_AARCH64)

#define ELF_EXEC_PAGESIZE PAGE_SIZE

#define ELF_ET_DYN_BASE (2 * TASK_SIZE / 3)

typedef unsigned long elf_greg_t;

/* x0-x30, sp, pc, pstate - matches the host's user_regs_struct. */
#define ELF_NGREG 34
typedef elf_greg_t elf_gregset_t[ELF_NGREG];

typedef struct {
	__uint128_t vregs[32];
	__u32 fpsr;
	__u32 fpcr;
} elf_fpregset_t;

/* The elfcore.h call site supplies no trailing semicolon. */
#define ELF_CORE_COPY_REGS(pr_reg, regs)		\
	memcpy(&(pr_reg), (regs)->regs.gp,		\
	       sizeof(unsigned long) * ELF_NGREG);

#define ELF_HWCAP (0)

#define ELF_PLATFORM "aarch64"

#define ELF_PLAT_INIT(regs, load_addr)

#endif
