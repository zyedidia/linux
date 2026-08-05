// SPDX-License-Identifier: GPL-2.0
/*
 * Module loading needs an AArch64 RELA relocator (with veneers for
 * out-of-range calls), which this port does not carry yet.  It could not be
 * exercised anyway: init_module() can only be called from userspace, and a
 * UML_NO_USERSPACE kernel has none.  Fail cleanly if generic code ever asks.
 */

#include <linux/elf.h>
#include <linux/moduleloader.h>
#include <linux/printk.h>

int apply_relocate_add(Elf64_Shdr *sechdrs, const char *strtab,
		       unsigned int symindex, unsigned int relsec,
		       struct module *me)
{
	pr_err("module %s: relocation is not supported on UML/arm64\n",
	       me->name);
	return -ENOEXEC;
}
