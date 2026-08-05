/* SPDX-License-Identifier: GPL-2.0 */
/*
 * setjmp/longjmp for AArch64 UML.
 *
 * The buffer layout must match struct __jmp_buf in sysdep/archsetjmp.h:
 *
 *	0x00  x19 x20 x21 x22 x23 x24 x25 x26 x27 x28
 *	0x50  x29 (fp)
 *	0x58  sp
 *	0x60  x30 (the "pc" slot new_thread() points at a handler)
 *	0x68  d8 d9 d10 d11 d12 d13 d14 d15
 *
 * d8-d15 are callee-saved and host-libc-compiled frames may keep values in
 * them across a thread switch, so they are part of the context.
 */

	.text
	.align 4
	.globl kernel_setjmp
	.type kernel_setjmp, @function
kernel_setjmp:
	stp	x19, x20, [x0, #0x00]
	stp	x21, x22, [x0, #0x10]
	stp	x23, x24, [x0, #0x20]
	stp	x25, x26, [x0, #0x30]
	stp	x27, x28, [x0, #0x40]
	mov	x2, sp
	stp	x29, x2,  [x0, #0x50]
	str	x30,      [x0, #0x60]
	stp	d8,  d9,  [x0, #0x68]
	stp	d10, d11, [x0, #0x78]
	stp	d12, d13, [x0, #0x88]
	stp	d14, d15, [x0, #0x98]
	mov	w0, #0
	ret

	.size kernel_setjmp,.-kernel_setjmp

	.align 4
	.globl kernel_longjmp
	.type kernel_longjmp, @function
kernel_longjmp:
	ldp	x19, x20, [x0, #0x00]
	ldp	x21, x22, [x0, #0x10]
	ldp	x23, x24, [x0, #0x20]
	ldp	x25, x26, [x0, #0x30]
	ldp	x27, x28, [x0, #0x40]
	ldp	x29, x2,  [x0, #0x50]
	mov	sp, x2
	ldr	x30,      [x0, #0x60]
	ldp	d8,  d9,  [x0, #0x68]
	ldp	d10, d11, [x0, #0x78]
	ldp	d12, d13, [x0, #0x88]
	ldp	d14, d15, [x0, #0x98]
	mov	w0, w1
	ret

	.size kernel_longjmp,.-kernel_longjmp
