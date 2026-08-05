// SPDX-License-Identifier: GPL-2.0
/*
 * Same shape as arch/x86/um/fault.c: point the PC at the fixup for a
 * faulting address, if an exception table entry exists for it.
 */

#include <arch.h>
#include <sysdep/ptrace.h>

/* These two are from asm-um/uaccess.h and linux/module.h, check them. */
struct exception_table_entry {
	unsigned long insn;
	unsigned long fixup;
};

const struct exception_table_entry *search_exception_tables(unsigned long add);

int arch_fixup(unsigned long address, struct uml_pt_regs *regs)
{
	const struct exception_table_entry *fixup;

	fixup = search_exception_tables(address);
	if (fixup) {
		UPT_IP(regs) = fixup->fixup;
		return 1;
	}
	return 0;
}
