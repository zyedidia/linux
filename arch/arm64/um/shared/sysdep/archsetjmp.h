/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARM64_UM_SYSDEP_ARCHSETJMP_H
#define __ARM64_UM_SYSDEP_ARCHSETJMP_H

/*
 * Buffer for UML's own setjmp/longjmp (arch/arm64/um/setjmp.S), which the
 * whole binary uses in place of the libc one via -Dsetjmp=kernel_setjmp.
 * Keep the layout in sync with the offsets hardcoded there.
 *
 * d8-d15 are in here because only the kernel side is guaranteed to stay off
 * the FP registers; host-libc-compiled objects between a setjmp and the
 * matching longjmp may keep live values in the callee-saved half of the
 * FPSIMD file.
 */
struct __jmp_buf {
	unsigned long __x19;
	unsigned long __x20;
	unsigned long __x21;
	unsigned long __x22;
	unsigned long __x23;
	unsigned long __x24;
	unsigned long __x25;
	unsigned long __x26;
	unsigned long __x27;
	unsigned long __x28;
	unsigned long __fp;	/* x29 */
	unsigned long __sp;
	unsigned long __pc;	/* x30 at save time, branch target in longjmp */
	unsigned long __fpregs[8];	/* d8-d15 */
};

typedef struct __jmp_buf jmp_buf[1];

#define JB_IP __pc
#define JB_SP __sp

/*
 * How far below the stack top a fresh thread's SP is placed.  The hardware
 * checks SP alignment on every SP-based access here (SCTLR_EL1.SA0 is set on
 * Linux hosts), so unlike x86 the initial SP must stay 16-byte aligned.
 */
#define JB_SP_OFFSET 16

unsigned long get_thread_reg(int reg, jmp_buf *buf);

#endif /* __ARM64_UM_SYSDEP_ARCHSETJMP_H */
