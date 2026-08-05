/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_SPINLOCK_H
#define __UM_ARM64_SPINLOCK_H

/*
 * The generic queued locks, built on the arch atomics.  The native arm64
 * qspinlock pulls in the LSE/alternatives machinery, which a UML binary
 * cannot patch at runtime.
 */
#include <asm/qspinlock.h>
#include <asm/qrwlock.h>

#endif
