/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_CMPXCHG_H
#define __UM_ARM64_CMPXCHG_H

#include <linux/compiler.h>
#include <asm/barrier.h>

/*
 * Compiler-builtin exchange primitives.
 *
 * Only the _relaxed forms are provided; the common fallback layer under
 * include/linux/atomic builds the acquire/release/full-barrier variants
 * out of these plus smp_mb(), which is the one composition known to satisfy
 * the kernel memory model.  (A seq_cst builtin compiles to ldaxr/stlxr,
 * which is not a full barrier in the LKMM sense.)
 *
 * With -mno-outline-atomics these compile to plain ll/sc loops.  A reservation
 * lost to host signal delivery just makes the stxr fail and the loop retry.
 */

#define arch_xchg_relaxed(ptr, x)					\
	__atomic_exchange_n((ptr), (x), __ATOMIC_RELAXED)

#define arch_cmpxchg_relaxed(ptr, old, new)				\
({									\
	__typeof__(*(ptr)) __old = (old);				\
	__atomic_compare_exchange_n((ptr), &__old, (new), 0,		\
				    __ATOMIC_RELAXED, __ATOMIC_RELAXED); \
	__old;								\
})

#define arch_cmpxchg64_relaxed		arch_cmpxchg_relaxed

/* Only has to be atomic w.r.t. this CPU, so relaxed is already stronger. */
#define arch_cmpxchg_local		arch_cmpxchg_relaxed
#define arch_cmpxchg64_local		arch_cmpxchg_relaxed

#endif /* __UM_ARM64_CMPXCHG_H */
