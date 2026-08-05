/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYS_SIGCONTEXT_ARM64_H
#define __SYS_SIGCONTEXT_ARM64_H

extern void get_regs_from_mc(struct uml_pt_regs *, mcontext_t *);
extern void get_mc_from_regs(struct uml_pt_regs *regs, mcontext_t *mc,
			     int single_stepping);
extern unsigned int mc_get_esr(mcontext_t *mc);

/*
 * The fault address sits directly in the mcontext; the ESR has to be fished
 * out of the __reserved record area, and is absent on older hosts.
 */
#define GET_FAULTINFO_FROM_MC(fi, mc) \
	{ \
		(fi).addr = (mc)->fault_address; \
		(fi).error_code = mc_get_esr(mc); \
		(fi).trap_no = 0; \
	}

#endif
