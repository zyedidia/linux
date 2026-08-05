// SPDX-License-Identifier: GPL-2.0
/*
 * Conversion between the host's signal mcontext and UML's register frame.
 * The gp[] layout is the same regs[31]/sp/pc/pstate sequence as the
 * mcontext, so the copies are straight loops.
 */

#include <string.h>
#include <sys/ucontext.h>
#include <asm/sigcontext.h>
#include <sysdep/ptrace.h>
#include <sysdep/mcontext.h>
#include <generated/user_constants.h>
#include <arch.h>

/* Hosts old enough to predate ESR export lack this in their headers. */
#ifndef ESR_MAGIC
#define ESR_MAGIC 0x45535201
struct esr_context {
	struct _aarch64_ctx head;
	unsigned long long esr;
};
#endif

void get_regs_from_mc(struct uml_pt_regs *regs, mcontext_t *mc)
{
	int i;

	for (i = 0; i < 31; i++)
		regs->gp[i] = mc->regs[i];
	regs->gp[HOST_SP] = mc->sp;
	regs->gp[HOST_IP] = mc->pc;
	regs->gp[HOST_PSTATE] = mc->pstate;
}

void get_mc_from_regs(struct uml_pt_regs *regs, mcontext_t *mc,
		      int single_stepping)
{
	int i;

	for (i = 0; i < 31; i++)
		mc->regs[i] = regs->gp[i];
	mc->sp = regs->gp[HOST_SP];
	mc->pc = regs->gp[HOST_IP];
	mc->pstate = regs->gp[HOST_PSTATE];
}

void mc_set_rip(void *_mc, void *target)
{
	mcontext_t *mc = _mc;

	mc->pc = (unsigned long)target;
}

/*
 * The ESR is tucked into the mcontext's __reserved area as one of the
 * tagged records described in the host's asm/sigcontext.h.  Hosts predating
 * ESR export just leave it out, in which case report 0.
 */
unsigned int mc_get_esr(mcontext_t *mc)
{
	struct _aarch64_ctx *ctx;
	unsigned char *base = mc->__reserved;
	size_t offset = 0;

	for (;;) {
		if (offset + sizeof(*ctx) > sizeof(mc->__reserved))
			return 0;

		ctx = (struct _aarch64_ctx *)(base + offset);
		if (ctx->magic == 0)
			return 0;
		if (ctx->magic == ESR_MAGIC)
			return ((struct esr_context *)ctx)->esr;

		if (ctx->size < sizeof(*ctx))
			return 0;
		offset += ctx->size;
	}
}
