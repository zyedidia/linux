/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/linkage.h>
#include <mpk.h>

#
# The gates into D1. These are the only WRPKRU instructions in UML, and
# arch/um/scripts/mpk-gadgets.sh checks that at link time.
#
# Every WRPKRU is followed by a check that it loaded the value that it was
# meant to, so that a jump to one with another value in %eax stops UML. A
# gate claims a free stack of the CPU's area, whose address is in the GS
# base, and calls the function there with the PKRU of D1. A host signal that
# arrives in between runs its handler with the PKRU of D0, on the signal
# stack, and the handler's own gates claim other stacks.
#

	.text
	.globl uml_mpk_gate_start
uml_mpk_gate_start:

# long uml_mpk_call(void *fn, a0, a1, a2, a3, a4, a5) calls
# fn(a0, a1, a2, a3, a4, a5) in D1. It must be called in D0, and fn must be in
# the .text.uml_mpk section.
SYM_FUNC_START(uml_mpk_call)
	movq	%rdi, %r11
	movq	%rdx, %r10
	movq	%rcx, %rdi

	xorl	%ecx, %ecx
	rdpkru
	cmpl	$UML_MPK_PKRU_D0, %eax
	jne	.Lmpk_abort
	movl	$UML_MPK_PKRU_D1, %eax
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	wrpkru
	cmpl	$UML_MPK_PKRU_D1, %eax
	jne	.Lmpk_abort

	# The function is checked with D1's PKRU, so that a jump past the
	# check runs it with D0's.
	leaq	__uml_mpk_text_start(%rip), %rax
	cmpq	%rax, %r11
	jb	.Lmpk_abort
	leaq	__uml_mpk_text_end(%rip), %rax
	cmpq	%rax, %r11
	jae	.Lmpk_abort

	# A single locked instruction claims the stack, so that a signal in
	# between cannot claim it too.
1:	movq	%gs:UML_MPK_CPU_SLOTS, %rax
	notq	%rax
	bsfq	%rax, %rcx
	jz	.Lmpk_abort
	cmpl	$UML_MPK_STACKS, %ecx
	jae	.Lmpk_abort
	lock btsq %rcx, %gs:UML_MPK_CPU_SLOTS
	jc	1b

	movq	%rsp, %rdx
	movq	%gs:UML_MPK_CPU_STACK(,%rcx,8), %rsp
	pushq	%rdx
	pushq	%rcx
	movq	%r8, %rcx
	movq	%r9, %r8
	movq	8(%rdx), %r9
	movq	%rdi, %rdx
	movq	%rsi, %rdi
	movq	%r10, %rsi
	call	*%r11
	# The stack's top word goes back to 0, which a signal that finds the
	# stack claimed before the caller's stack pointer is stored reads.
	popq	%rcx
	popq	%rdx
	movq	$0, -8(%rsp)
	lock btrq %rcx, %gs:UML_MPK_CPU_SLOTS
	movq	%rdx, %rsp

	movq	%rax, %r10
	movl	$UML_MPK_PKRU_D0, %eax
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	wrpkru
	cmpl	$UML_MPK_PKRU_D0, %eax
	jne	.Lmpk_abort
	movq	%r10, %rax
	RET

	# Every check fails on the caller's stack, so D0 code can panic there.
.Lmpk_abort:
	movl	$UML_MPK_PKRU_D0, %eax
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	wrpkru
	cmpl	$UML_MPK_PKRU_D0, %eax
	jne	.Lmpk_abort
	andq	$-16, %rsp
	call	uml_mpk_gate_failed
	ud2
SYM_FUNC_END(uml_mpk_call)

# This calls fn the same way without switching the PKRU or the stack, for a
# UML that runs without protection keys.
SYM_FUNC_START(uml_mpk_call_plain)
	movq	%rdi, %r11
	movq	%rsi, %rdi
	movq	%rdx, %rsi
	movq	%rcx, %rdx
	movq	%r8, %rcx
	movq	%r9, %r8
	movq	8(%rsp), %r9
	jmp	*%r11
SYM_FUNC_END(uml_mpk_call_plain)

# long uml_mpk_syscall(nr, a0, a1, a2, a3, a4, a5) makes a system call that
# the sandbox allows only from here, such as pkey_mprotect, and returns its
# result or a negative errno. It must be called in D1.
SYM_FUNC_START(uml_mpk_syscall)
	movq	%rdx, %r10
	movq	%rcx, %r11
	xorl	%ecx, %ecx
	rdpkru
	cmpl	$UML_MPK_PKRU_D1, %eax
	jne	.Lmpk_abort
	movq	%rdi, %rax
	movq	%rsi, %rdi
	movq	%r10, %rsi
	movq	%r11, %rdx
	movq	%r8, %r10
	movq	%r9, %r8
	movq	8(%rsp), %r9
	syscall
	.globl uml_mpk_syscall_ip
uml_mpk_syscall_ip:
	RET
SYM_FUNC_END(uml_mpk_syscall)

# void uml_mpk_fail(const char *msg) panics with msg from D1. It gives back
# the gate's stack, and the panic runs in D0 on the stack of the gate's
# caller, below what the caller uses, so that no D0 code runs with the PKRU
# of D1.
SYM_FUNC_START(uml_mpk_fail)
	movq	%rdi, %r11
	movq	%rsp, %r10
	xorl	%ecx, %ecx
1:	movq	%gs:UML_MPK_CPU_STACK(,%rcx,8), %rax
	cmpq	%rax, %rsp
	jae	2f
	leaq	-UML_MPK_STACK_SIZE(%rax), %rdx
	cmpq	%rdx, %rsp
	jb	2f
	movq	-8(%rax), %r10
	movq	$0, -8(%rax)
	lock btrq %rcx, %gs:UML_MPK_CPU_SLOTS
	jmp	3f
2:	incl	%ecx
	cmpl	$UML_MPK_STACKS, %ecx
	jb	1b
3:	movl	$UML_MPK_PKRU_D0, %eax
	xorl	%ecx, %ecx
	xorl	%edx, %edx
	wrpkru
	cmpl	$UML_MPK_PKRU_D0, %eax
	jne	3b
	leaq	-256(%r10), %rsp
	andq	$-16, %rsp
	movq	%r11, %rsi
	leaq	.Lmpk_fail_fmt(%rip), %rdi
	xorl	%eax, %eax
	call	panic
	ud2
SYM_FUNC_END(uml_mpk_fail)

# void uml_mpk_sigreturn(void *uc, unsigned long ssp) returns from a signal
# through the frame whose ucontext is at uc, which D1 built in the window. It
# must be called in D1. Ssp is the shadow stack pointer at the start of the
# handler, or 0 without shadow stacks. The handler's restorer is then on top
# of the shadow stack, with the kernel's token below it, which sigreturn
# needs on top.
SYM_FUNC_START(uml_mpk_sigreturn)
	xorl	%ecx, %ecx
	rdpkru
	cmpl	$UML_MPK_PKRU_D1, %eax
	jne	.Lmpk_sigreturn_failed
	testq	%rsi, %rsi
	jz	1f
	xorl	%eax, %eax
	rdsspq	%rax
	leaq	8(%rsi), %rcx
	subq	%rax, %rcx
	shrq	$3, %rcx
	cmpq	$255, %rcx
	ja	.Lmpk_sigreturn_failed
	incsspq	%rcx
	# The kernel finds the frame one word below the stack pointer.
1:	movq	%rdi, %rsp
	movl	$15, %eax			# __NR_rt_sigreturn
	syscall
.Lmpk_sigreturn_failed:
	leaq	.Lmpk_sigreturn_msg(%rip), %rdi
	call	uml_mpk_fail
	ud2
SYM_FUNC_END(uml_mpk_sigreturn)

	.globl uml_mpk_gate_end
uml_mpk_gate_end:

	.section .rodata.str1.1, "aMS", @progbits, 1
.Lmpk_fail_fmt:
	.asciz	"%s"
.Lmpk_sigreturn_msg:
	.asciz	"Returning from a signal failed in a protection key gate"
