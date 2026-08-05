/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __FAULTINFO_ARM64_H
#define __FAULTINFO_ARM64_H

/*
 * The arch-specific fault information UML carries around, filled in from the
 * host's SIGSEGV mcontext.  Layout mirrors the x86 one so the shared code's
 * expectations about the fields hold.
 */
struct faultinfo {
	/* ESR_EL1 as exposed via the signal frame's esr_context, 0 if the
	 * host did not provide one. */
	int error_code;
	unsigned long addr;
	int trap_no;
};

#define FAULT_ADDRESS(fi) ((fi).addr)

/*
 * WnR (bit 6) is only meaningful for data aborts, EC (bits 31:26)
 * 0b100100/0b100101.  Without an ESR everything reads as 0, i.e. a read
 * fault, which only makes a faulting write oops as a read - acceptable for
 * a kernel that never runs userspace.
 */
#define FAULT_WRITE(fi) \
	((((unsigned int)(fi).error_code >> 26) & 0x3e) == 0x24 && \
	 ((fi).error_code & (1 << 6)))

/*
 * Any host SIGSEGV is a page fault as far as UML is concerned; there is no
 * x86-style trap number to disambiguate.
 */
#define SEGV_IS_FIXABLE(fi) (1)

#define PTRACE_FULL_FAULTINFO 1

/*
 * Plant the recovery address for __get/put_kernel_nofault: stash a label in
 * current->thread.segv_continue and fall through with _faulted = 0.  If the
 * access that follows faults, the SEGV handler moves the PC here (see
 * segv() in arch/um/kernel/trap.c), which sets _faulted = 1.  x16 is IP0,
 * free for scratch use.
 */
#define ___backtrack_faulted(_faulted)					\
	asm volatile (							\
		"	adr	x16, 2f\n"				\
		"	str	x16, %1\n"				\
		"	mov	%w0, #0\n"				\
		"	b	3f\n"					\
		"2:	mov	%w0, #1\n"				\
		"3:\n"							\
		: "=r" (_faulted),					\
		  "=m" (current->thread.segv_continue) ::		\
		"x16")

#endif
