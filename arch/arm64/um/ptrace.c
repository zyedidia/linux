// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace glue for AArch64 UML.  Guest ptrace only means something for guest
 * processes, which need userspace support; everything here exists so the
 * arch-neutral ptrace/regset code links, and answers "no such register" if
 * it is ever reached.
 */

#include <linux/sched.h>
#include <linux/elf.h>
#include <linux/regset.h>
#include <asm/ptrace-generic.h>

unsigned long getreg(struct task_struct *child, int regno)
{
	return 0;
}

int putreg(struct task_struct *child, int regno, unsigned long value)
{
	return -EIO;
}

int peek_user(struct task_struct *child, long addr, long data)
{
	return -EIO;
}

int poke_user(struct task_struct *child, long addr, long data)
{
	return -EIO;
}

long subarch_ptrace(struct task_struct *child, long request,
		    unsigned long addr, unsigned long data)
{
	return -EIO;
}

static int genregs_get(struct task_struct *target,
		       const struct user_regset *regset,
		       struct membuf to)
{
	return membuf_write(&to, &target->thread.regs.regs.gp,
			    sizeof(target->thread.regs.regs.gp));
}

static const struct user_regset uml_arm64_regsets[] = {
	[REGSET_GENERAL] = {
		USER_REGSET_NOTE_TYPE(PRSTATUS),
		.n		= sizeof(struct uml_pt_regs) / sizeof(long),
		.size		= sizeof(long),
		.align		= sizeof(long),
		.regset_get	= genregs_get,
	},
};

static const struct user_regset_view uml_arm64_view = {
	.name		= "aarch64",
	.e_machine	= EM_AARCH64,
	.regsets	= uml_arm64_regsets,
	.n		= ARRAY_SIZE(uml_arm64_regsets),
};

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
	return &uml_arm64_view;
}

/* TLS is only managed for guest processes, which cannot exist here. */
int arch_set_tls(struct task_struct *new, unsigned long tls)
{
	return -EINVAL;
}

void clear_flushed_tls(struct task_struct *task)
{
}

void arch_switch_to(struct task_struct *to)
{
}
