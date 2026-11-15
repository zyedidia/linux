// SPDX-License-Identifier: GPL-2.0
/*
 * The kernel side of UML's memory protection keys. The state that the gates
 * trust outside the window is set before the kernel starts and is read-only
 * after boot.
 */

#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <asm/mpk.h>
#include <os.h>

int uml_mpk_enabled __ro_after_init;
unsigned long uml_mpk_window __ro_after_init;
unsigned long uml_mpk_fpregs_off __ro_after_init;
unsigned long uml_mpk_pkru_off __ro_after_init;
unsigned char uml_mpk_sw_bytes[48] __ro_after_init;
unsigned int uml_mpk_magic2 __ro_after_init;
unsigned long uml_mpk_xstate_size __ro_after_init;
unsigned long uml_mpk_vm_start __ro_after_init;
unsigned long uml_mpk_vm_end __ro_after_init;

/*
 * The number of module texts that D1 holds. While it is 0, the page table
 * sync needs no gate to know that a page is not module text.
 */
static atomic_t uml_mpk_execs = ATOMIC_INIT(0);

int uml_mpk_exec_add(void *start, size_t size)
{
	int err;

	if (!uml_mpk_enabled)
		return 0;
	err = uml_mpk_gate(uml_mpk_exec_add_d1, start, size);
	if (!err)
		atomic_inc(&uml_mpk_execs);
	return err;
}

int uml_mpk_exec_seal(void *start, size_t size)
{
	if (!uml_mpk_enabled || !start)
		return 0;
	return uml_mpk_gate(uml_mpk_exec_seal_d1, start, size);
}

void uml_mpk_exec_remove(void *start)
{
	if (!uml_mpk_enabled || !start || !atomic_read(&uml_mpk_execs))
		return;
	if (uml_mpk_gate(uml_mpk_exec_remove_d1, start) > 0)
		atomic_dec(&uml_mpk_execs);
}

int uml_mpk_exec_sealed(unsigned long virt)
{
	if (!atomic_read(&uml_mpk_execs))
		return 0;
	return uml_mpk_gate(uml_mpk_exec_sealed_d1, virt);
}

void __noreturn uml_mpk_gate_failed(void)
{
	panic("Protection key gate check failed");
}

#ifdef CONFIG_UML_MPK_TEST

#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "uml_mpk."

static char *uml_mpk_test;
module_param_named(test, uml_mpk_test, charp, 0400);

static long __uml_mpk_entry uml_mpk_test_nop(void)
{
	return 0;
}

static long __uml_mpk_entry uml_mpk_test_alloc(unsigned long size)
{
	return (long)uml_mpk_alloc(size);
}

static long uml_mpk_test_outside(void)
{
	return 0;
}

static void uml_mpk_test_bench(void)
{
	unsigned int i, n = 1000000;
	u64 start, end;

	start = ktime_get_ns();
	for (i = 0; i < n; i++)
		uml_mpk_gate(uml_mpk_test_nop, 0);
	end = ktime_get_ns();
	pr_info("uml_mpk: a gate takes %llu ns\n", (end - start) / n);

	start = ktime_get_ns();
	for (i = 0; i < n; i++)
		uml_mpk_call_plain(uml_mpk_test_nop, 0, 0, 0, 0, 0, 0);
	end = ktime_get_ns();
	pr_info("uml_mpk: a call without a gate takes %llu ns\n",
		(end - start) / n);
}

static void uml_mpk_test_result(const char *name, bool ok)
{
	pr_info("uml_mpk: test %s %s\n", name, ok ? "passed" : "failed");
}

/* The tests that pass with a panic report a failure if they return. */
static int __init uml_mpk_run_test(void)
{
	const char *t = uml_mpk_test;
	int *p;

	if (!t)
		return 0;
	if (!uml_mpk_enabled) {
		pr_info("uml_mpk: test %s needs protection keys\n", t);
		return 0;
	}

	if (!strcmp(t, "bench")) {
		uml_mpk_test_bench();
		return 0;
	}
	if (!strcmp(t, "frame")) {
		uml_mpk_test_result(t, !uml_mpk_test_frame());
		return 0;
	}
	if (!strcmp(t, "regs")) {
		uml_mpk_test_result(t, !uml_mpk_test_regs());
		return 0;
	}
	if (!strcmp(t, "maps")) {
		uml_mpk_test_result(t, !uml_mpk_test_maps());
		return 0;
	}
	if (!strcmp(t, "unmap")) {
		uml_mpk_test_result(t, os_unmap_memory((void *)uml_mpk_window,
							4096) == -EPERM);
		return 0;
	}

	p = (int *)uml_mpk_gate(uml_mpk_test_alloc, sizeof(*p));
	if (!p) {
		uml_mpk_test_result(t, false);
		return 0;
	}

	if (!strcmp(t, "write")) {
		pr_info("uml_mpk: writing %px from D0, which should panic\n", p);
		WRITE_ONCE(*p, 1);
	} else if (!strcmp(t, "read")) {
		pr_info("uml_mpk: reading %px from D0, which should panic\n", p);
		pr_info("uml_mpk: read %d\n", READ_ONCE(*p));
	} else if (!strcmp(t, "gate")) {
		pr_info("uml_mpk: calling a function outside the gate section, which should panic\n");
		uml_mpk_gate(uml_mpk_test_outside, 0);
	} else if (!strcmp(t, "sandbox")) {
		pr_info("uml_mpk: unmapping the window from D0, which the sandbox should stop\n");
		uml_mpk_test_munmap();
	} else if (!strcmp(t, "exec")) {
		pr_info("uml_mpk: mapping executable memory from D0, which the sandbox should stop\n");
		uml_mpk_test_exec();
	} else {
		pr_info("uml_mpk: unknown test %s\n", t);
		return 0;
	}
	uml_mpk_test_result(t, false);
	return 0;
}
late_initcall(uml_mpk_run_test);

#endif
