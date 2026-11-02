/* SPDX-License-Identifier: GPL-2.0 */
#
# arch/x86_64/setjmp.S
#
# setjmp/longjmp for the x86-64 architecture
#

#
# The jmp_buf is assumed to contain the following, in order:
#	%rbx
#	%rsp (post-return)
#	%rbp
#	%r12
#	%r13
#	%r14
#	%r15
#	<return address>
#	<address of a shadow stack restore token>
#
# With shadow stacks, kernel_longjmp pops its own return address, switches
# to the shadow stack of the target and leaves a restore token on the old
# one, just below the shadow stack pointer of its caller's frame. A jmp_buf
# that kernel_setjmp filled therefore points to a valid token only once
# kernel_longjmp has been called from the frame that called kernel_setjmp.
# That call must be a real call rather than a tail jump, so that the return
# address it pops is its own.
#

	.text
	.align 4
	.globl kernel_setjmp
	.type kernel_setjmp, @function
kernel_setjmp:
	pop  %rsi			# Return address, and adjust the stack
	xorl %eax,%eax			# Return value
	movq %rbx,(%rdi)
	movq %rsp,8(%rdi)		# Post-return %rsp!
	push %rsi			# Make the call/return stack happy
	movq %rbp,16(%rdi)
	movq %r12,24(%rdi)
	movq %r13,32(%rdi)
	movq %r14,40(%rdi)
	movq %r15,48(%rdi)
	movq %rsi,56(%rdi)		# Return address
#ifdef CONFIG_UML_SHADOW_STACK
	xorl %ecx,%ecx
	rdsspq %rcx			# This is zero without a shadow stack.
	movq %rcx,64(%rdi)		# Longjmp leaves the token here.
#endif
	RET

	.size kernel_setjmp,.-kernel_setjmp

	.text
	.align 4
	.globl kernel_longjmp
	.type kernel_longjmp, @function
kernel_longjmp:
	movl %esi,%eax			# Return value (int)
#ifdef CONFIG_UML_SHADOW_STACK
	xorl %ecx,%ecx
	rdsspq %rcx
	testq %rcx,%rcx
	jz 1f
	movl $1,%ecx
	incsspq %rcx			# Pop the return address.
	movq 64(%rdi),%rcx
	rstorssp (%rcx)			# Switch to the target's shadow stack.
	saveprevssp			# Leave a token on the old one.
1:
#endif
	movq (%rdi),%rbx
	movq 8(%rdi),%rsp
	movq 16(%rdi),%rbp
	movq 24(%rdi),%r12
	movq 32(%rdi),%r13
	movq 40(%rdi),%r14
	movq 48(%rdi),%r15
	jmp *56(%rdi)

	.size kernel_longjmp,.-kernel_longjmp
