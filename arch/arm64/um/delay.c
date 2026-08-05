// SPDX-License-Identifier: GPL-2.0-only
/*
 * Calibrated-loop delays for AArch64 UML, mirroring arch/x86/um/delay.c.
 * Plain cycle burning: this is a host process, so there is no fixed-rate
 * counter worth wiring up to lpj.
 */

#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <asm/param.h>

void __delay(unsigned long loops)
{
	while (loops--)
		asm volatile("" ::: "memory");
}
EXPORT_SYMBOL(__delay);

inline void __const_udelay(unsigned long xloops)
{
	unsigned long long loops;

	loops = (unsigned long long)xloops * loops_per_jiffy * HZ;

	__delay(loops >> 32);
}
EXPORT_SYMBOL(__const_udelay);

void __udelay(unsigned long usecs)
{
	__const_udelay(usecs * 0x10c7ul);	/* 2**32 / 1000000 (rounded up) */
}
EXPORT_SYMBOL(__udelay);

void __ndelay(unsigned long nsecs)
{
	__const_udelay(nsecs * 0x5ul);		/* 2**32 / 1000000000 (rounded up) */
}
EXPORT_SYMBOL(__ndelay);
