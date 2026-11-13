/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ASM_MPK_H
#define __UM_ASM_MPK_H

#include <linux/irqflags.h>
#include <mpk.h>

/*
 * Signals are off for the whole gate, so that no interrupt handler runs on
 * the CPU while it holds a lock of D1.
 */
static inline long __uml_mpk_gate(void *fn, unsigned long a0, unsigned long a1,
				  unsigned long a2, unsigned long a3,
				  unsigned long a4, unsigned long a5)
{
	unsigned long flags;
	long ret;

	local_irq_save(flags);
	if (uml_mpk_enabled)
		ret = uml_mpk_call(fn, a0, a1, a2, a3, a4, a5);
	else
		ret = uml_mpk_call_plain(fn, a0, a1, a2, a3, a4, a5);
	local_irq_restore(flags);
	return ret;
}

#define __uml_mpk_args(a0, a1, a2, a3, a4, a5, ...)			\
	(unsigned long)(a0), (unsigned long)(a1), (unsigned long)(a2),	\
	(unsigned long)(a3), (unsigned long)(a4), (unsigned long)(a5)

/*
 * Run a function of the .text.uml_mpk section in D1 with up to six integer
 * or pointer arguments, and return its result.
 */
#define uml_mpk_gate(fn, ...)						\
	__uml_mpk_gate((void *)(fn),					\
		       __uml_mpk_args(__VA_ARGS__, 0, 0, 0, 0, 0, 0))

#endif /* __UM_ASM_MPK_H */
