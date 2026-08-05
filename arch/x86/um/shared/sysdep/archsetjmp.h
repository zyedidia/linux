/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __X86_UM_SYSDEP_ARCHSETJMP_H
#define __X86_UM_SYSDEP_ARCHSETJMP_H

#ifdef __i386__
#include "archsetjmp_32.h"
#else
#include "archsetjmp_64.h"
#endif

/*
 * How far below the stack top a fresh thread's SP starts.  One word keeps
 * the frame address after the implied "call" 16-byte aligned.
 */
#define JB_SP_OFFSET (sizeof(void *))

unsigned long get_thread_reg(int reg, jmp_buf *buf);

#endif /* __X86_UM_SYSDEP_ARCHSETJMP_H */
