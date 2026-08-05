/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_BARRIER_H
#define __UM_ARM64_BARRIER_H

/*
 * Standalone version of the arm64 barriers.  The native header leans on the
 * alternatives framework for spectre and errata workarounds, which a UML
 * binary neither has nor needs - the host kernel already applies those on
 * every entry.  All of these execute fine at EL0.
 */

#define nop()		asm volatile("nop")

#define sev()		asm volatile("sev" : : : "memory")
#define wfe()		asm volatile("wfe" : : : "memory")

#define isb()		asm volatile("isb" : : : "memory")
#define dmb(opt)	asm volatile("dmb " #opt : : : "memory")
#define dsb(opt)	asm volatile("dsb " #opt : : : "memory")

#define mb()		dsb(sy)
#define rmb()		dsb(ld)
#define wmb()		dsb(st)

/* Device memory can be mapped in via VFIO, so order against outer-shareable. */
#define dma_mb()	dmb(osh)
#define dma_rmb()	dmb(oshld)
#define dma_wmb()	dmb(oshst)

#define __smp_mb()	dmb(ish)
#define __smp_rmb()	dmb(ishld)
#define __smp_wmb()	dmb(ishst)

#include <asm-generic/barrier.h>

#endif /* __UM_ARM64_BARRIER_H */
