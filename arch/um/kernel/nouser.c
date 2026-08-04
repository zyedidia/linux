// SPDX-License-Identifier: GPL-2.0
/*
 * Stand-ins for the userspace-facing arch hooks, for CONFIG_UML_NO_USERSPACE.
 *
 * There is no user address space in such a build, so every access to one has
 * to fail rather than walk a page table that cannot exist.  Generic code still
 * references these, which is why they are defined at all - nothing here is
 * ever reached, because nothing ever runs in user mode.
 */

#include <linux/errno.h>
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

#include <asm/futex.h>
#include <asm/mmu_context.h>
#include <asm/processor.h>

unsigned long raw_copy_from_user(void *to, const void __user *from,
				 unsigned long n)
{
	return n;
}
EXPORT_SYMBOL(raw_copy_from_user);

unsigned long raw_copy_to_user(void __user *to, const void *from,
			       unsigned long n)
{
	return n;
}
EXPORT_SYMBOL(raw_copy_to_user);

unsigned long __clear_user(void __user *mem, unsigned long len)
{
	return len;
}
EXPORT_SYMBOL(__clear_user);

long strncpy_from_user(char *dst, const char __user *src, long count)
{
	return -EFAULT;
}
EXPORT_SYMBOL(strncpy_from_user);

long strnlen_user(const char __user *str, long len)
{
	return 0;
}
EXPORT_SYMBOL(strnlen_user);

#ifdef CONFIG_FUTEX
int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval, u32 __user *uaddr)
{
	return -EFAULT;
}
EXPORT_SYMBOL(arch_futex_atomic_op_inuser);

int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
				  u32 oldval, u32 newval)
{
	return -EFAULT;
}
EXPORT_SYMBOL(futex_atomic_cmpxchg_inatomic);
#endif

/*
 * begin_new_exec() calls this unconditionally, so it has to exist even though
 * no exec can ever get that far without a binfmt handler.
 */
void flush_thread(void)
{
}

/*
 * Normally this starts a stub process to own the new address space.  Here
 * there is nothing to start, but um_tlb_sync() still wants the lock - only
 * init_mm should ever reach it, and that one is initialised statically.
 */
int init_new_context(struct task_struct *task, struct mm_struct *mm)
{
	mutex_init(&mm->context.turnstile);
	spin_lock_init(&mm->context.sync_tlb_lock);

	return 0;
}

void destroy_context(struct mm_struct *mm)
{
}
