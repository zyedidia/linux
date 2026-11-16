// SPDX-License-Identifier: GPL-2.0
#include <linux/cache.h>
#include <linux/init.h>
#include <asm/cpufeature.h>

/* Set when the host has fast short rep movsb, which memcpy then uses. */
bool uml_fsrm __ro_after_init;

static int __init uml_string_init(void)
{
	uml_fsrm = boot_cpu_has(X86_FEATURE_FSRM);
	return 0;
}
early_initcall(uml_string_init);
