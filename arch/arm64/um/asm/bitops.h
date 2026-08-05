/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_BITOPS_H
#define __UM_ARM64_BITOPS_H

#ifndef _LINUX_BITOPS_H
#error only <linux/bitops.h> can be included directly
#endif

#include <linux/compiler.h>

/*
 * The same composition the native arm64 bitops.h uses: compiler builtins for
 * the scans, the atomic_long-based generics for the atomic ops (which land on
 * the builtin atomics from asm/atomic.h here).
 */
#include <asm-generic/bitops/builtin-__ffs.h>
#include <asm-generic/bitops/builtin-ffs.h>
#include <asm-generic/bitops/builtin-__fls.h>
#include <asm-generic/bitops/builtin-fls.h>
#include <asm-generic/bitops/ffz.h>
#include <asm-generic/bitops/fls64.h>
#include <asm-generic/bitops/sched.h>
#include <asm-generic/bitops/hweight.h>

#include <asm-generic/bitops/atomic.h>
#include <asm-generic/bitops/lock.h>
#include <asm-generic/bitops/non-atomic.h>
#include <asm-generic/bitops/le.h>
#include <asm-generic/bitops/ext2-atomic-setbit.h>

#endif /* __UM_ARM64_BITOPS_H */
