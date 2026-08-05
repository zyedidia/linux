// SPDX-License-Identifier: GPL-2.0
/*
 * There is nothing to probe: any AArch64 host that can run this binary can
 * run the kernel built into it.
 */

#include <arch.h>
#include <sysdep/ptrace.h>

void arch_check_bugs(void)
{
}

void arch_examine_signal(int sig, struct uml_pt_regs *regs)
{
}
