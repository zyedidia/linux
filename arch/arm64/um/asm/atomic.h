/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_ATOMIC_H
#define __UM_ARM64_ATOMIC_H

#include <linux/compiler.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/cmpxchg.h>

/*
 * Compiler-builtin atomics instead of the native arm64 implementation, whose
 * LSE/ll-sc selection happens through runtime code patching that a UML binary
 * cannot perform.  As in cmpxchg.h, only relaxed forms are defined here; the
 * generic fallbacks add the ordered variants with explicit fences.
 */

#define ATOMIC64_INIT(i) { (i) }

#define arch_atomic_read(v)	READ_ONCE((v)->counter)
#define arch_atomic_set(v, i)	WRITE_ONCE(((v)->counter), (i))

#define ATOMIC_OP(op, c_op)						\
static __always_inline void arch_atomic_##op(int i, atomic_t *v)	\
{									\
	__atomic_fetch_##c_op(&v->counter, i, __ATOMIC_RELAXED);	\
}									\
static __always_inline int arch_atomic_fetch_##op##_relaxed(int i, atomic_t *v) \
{									\
	return __atomic_fetch_##c_op(&v->counter, i, __ATOMIC_RELAXED);	\
}

ATOMIC_OP(add, add)
ATOMIC_OP(sub, sub)
ATOMIC_OP(and, and)
ATOMIC_OP(or, or)
ATOMIC_OP(xor, xor)

#undef ATOMIC_OP

static __always_inline void arch_atomic_andnot(int i, atomic_t *v)
{
	__atomic_fetch_and(&v->counter, ~i, __ATOMIC_RELAXED);
}

static __always_inline int arch_atomic_fetch_andnot_relaxed(int i, atomic_t *v)
{
	return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_RELAXED);
}

static __always_inline int arch_atomic_add_return_relaxed(int i, atomic_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_RELAXED);
}

static __always_inline int arch_atomic_sub_return_relaxed(int i, atomic_t *v)
{
	return __atomic_sub_fetch(&v->counter, i, __ATOMIC_RELAXED);
}

#define arch_atomic_fetch_add_relaxed	arch_atomic_fetch_add_relaxed
#define arch_atomic_fetch_sub_relaxed	arch_atomic_fetch_sub_relaxed
#define arch_atomic_fetch_and_relaxed	arch_atomic_fetch_and_relaxed
#define arch_atomic_fetch_andnot_relaxed arch_atomic_fetch_andnot_relaxed
#define arch_atomic_fetch_or_relaxed	arch_atomic_fetch_or_relaxed
#define arch_atomic_fetch_xor_relaxed	arch_atomic_fetch_xor_relaxed
#define arch_atomic_add_return_relaxed	arch_atomic_add_return_relaxed
#define arch_atomic_sub_return_relaxed	arch_atomic_sub_return_relaxed
#define arch_atomic_andnot		arch_atomic_andnot

#define ATOMIC64_OP(op, c_op)						\
static __always_inline void arch_atomic64_##op(s64 i, atomic64_t *v)	\
{									\
	__atomic_fetch_##c_op(&v->counter, i, __ATOMIC_RELAXED);	\
}									\
static __always_inline s64 arch_atomic64_fetch_##op##_relaxed(s64 i, atomic64_t *v) \
{									\
	return __atomic_fetch_##c_op(&v->counter, i, __ATOMIC_RELAXED);	\
}

ATOMIC64_OP(add, add)
ATOMIC64_OP(sub, sub)
ATOMIC64_OP(and, and)
ATOMIC64_OP(or, or)
ATOMIC64_OP(xor, xor)

#undef ATOMIC64_OP

static __always_inline void arch_atomic64_andnot(s64 i, atomic64_t *v)
{
	__atomic_fetch_and(&v->counter, ~i, __ATOMIC_RELAXED);
}

static __always_inline s64 arch_atomic64_fetch_andnot_relaxed(s64 i, atomic64_t *v)
{
	return __atomic_fetch_and(&v->counter, ~i, __ATOMIC_RELAXED);
}

static __always_inline s64 arch_atomic64_add_return_relaxed(s64 i, atomic64_t *v)
{
	return __atomic_add_fetch(&v->counter, i, __ATOMIC_RELAXED);
}

static __always_inline s64 arch_atomic64_sub_return_relaxed(s64 i, atomic64_t *v)
{
	return __atomic_sub_fetch(&v->counter, i, __ATOMIC_RELAXED);
}

#define arch_atomic64_read(v)		READ_ONCE((v)->counter)
#define arch_atomic64_set(v, i)		WRITE_ONCE(((v)->counter), (i))

#define arch_atomic64_fetch_add_relaxed	arch_atomic64_fetch_add_relaxed
#define arch_atomic64_fetch_sub_relaxed	arch_atomic64_fetch_sub_relaxed
#define arch_atomic64_fetch_and_relaxed	arch_atomic64_fetch_and_relaxed
#define arch_atomic64_fetch_andnot_relaxed arch_atomic64_fetch_andnot_relaxed
#define arch_atomic64_fetch_or_relaxed	arch_atomic64_fetch_or_relaxed
#define arch_atomic64_fetch_xor_relaxed	arch_atomic64_fetch_xor_relaxed
#define arch_atomic64_add_return_relaxed arch_atomic64_add_return_relaxed
#define arch_atomic64_sub_return_relaxed arch_atomic64_sub_return_relaxed
#define arch_atomic64_andnot		arch_atomic64_andnot

#endif /* __UM_ARM64_ATOMIC_H */
