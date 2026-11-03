/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ASM_CFI_H
#define __UM_ASM_CFI_H

#include <linux/bug.h>

struct pt_regs;

#ifdef CONFIG_CFI
enum bug_trap_type handle_cfi_failure(struct pt_regs *regs);
#else
static inline enum bug_trap_type handle_cfi_failure(struct pt_regs *regs)
{
	return BUG_TRAP_TYPE_NONE;
}
#endif

#endif /* __UM_ASM_CFI_H */
