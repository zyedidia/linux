/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _ASM_UM_FPU_API_H
#define _ASM_UM_FPU_API_H

#include <linux/types.h>

/* Copyright (c) 2020 Cambridge Greys Ltd
 * Copyright (c) 2020 Red Hat Inc.
 */

/*
 * Kernel code can use the FPU without saving its state. User processes run
 * in host processes of their own, and the host saves the CPU thread's state
 * around each signal handler. Every switch between kernel tasks is a function
 * call, across which the x86 calling convention keeps no FPU or vector
 * register, and nothing in the kernel changes the FPU control state.
 */

#define kernel_fpu_begin() (void)0
#define kernel_fpu_end() (void)0

static inline bool irq_fpu_usable(void)
{
	return true;
}


#endif
