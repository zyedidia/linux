/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_PROCESSOR_H
#define __UM_PROCESSOR_H
#include <linux/time-internal.h>

/* include faultinfo structure */
#include <sysdep/faultinfo.h>

struct arch_thread {
	struct faultinfo faultinfo;
};

#define INIT_ARCH_THREAD { .faultinfo = { 0, 0, 0 } }

/* How many stack words sysrq's stack dump prints per line. */
#define STACKSLOTS_PER_LINE 4

static inline void arch_flush_thread(struct arch_thread *thread)
{
}

static inline void arch_copy_thread(struct arch_thread *from,
				    struct arch_thread *to)
{
}

#define current_sp() ({ void *sp; __asm__("mov %0, sp" : "=r" (sp) : ); sp; })
#define current_bp() ({ unsigned long bp; __asm__("mov %0, x29" : "=r" (bp) : ); bp; })

#define KSTK_EIP(tsk) KSTK_REG(tsk, HOST_IP)
#define KSTK_ESP(tsk) KSTK_REG(tsk, HOST_SP)
#define KSTK_EBP(tsk) KSTK_REG(tsk, HOST_FP)

#define ARCH_IS_STACKGROW(address) \
	(address + 65536 + 32 * sizeof(unsigned long) >= UPT_SP(&current->thread.regs.regs))

static __always_inline void cpu_relax(void)
{
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL)
		time_travel_ndelay(1);
	else
		__asm__ __volatile__("yield" : : : "memory");
}

#define task_pt_regs(t) (&(t)->thread.regs)

#include <asm/processor-generic.h>

#endif
