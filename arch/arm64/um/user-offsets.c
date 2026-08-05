// SPDX-License-Identifier: GPL-2.0
/*
 * Generates include/generated/user_constants.h: the layout of the host's
 * register frame plus a few host constants, for use in code that cannot see
 * the host headers directly.
 */

#include <stdio.h>
#include <stddef.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <linux/kbuild.h>

#define DEFINE_LONGS(sym, val) \
	DEFINE(sym, val / sizeof(unsigned long))

/* workaround for a warning with -Wmissing-prototypes */
void foo(void);

void foo(void)
{
	/*
	 * x0-x30 sit at their own register number in user_regs_struct, so
	 * only the named slots need constants.
	 */
	DEFINE_LONGS(HOST_SP, offsetof(struct user_regs_struct, sp));
	DEFINE_LONGS(HOST_IP, offsetof(struct user_regs_struct, pc));
	DEFINE_LONGS(HOST_PSTATE, offsetof(struct user_regs_struct, pstate));
	DEFINE(HOST_FP, 29);
	DEFINE(HOST_LR, 30);

	DEFINE(UM_FRAME_SIZE, sizeof(struct user_regs_struct));
	DEFINE(UM_POLLIN, POLLIN);
	DEFINE(UM_POLLPRI, POLLPRI);
	DEFINE(UM_POLLOUT, POLLOUT);

	DEFINE(UM_PROT_READ, PROT_READ);
	DEFINE(UM_PROT_WRITE, PROT_WRITE);
	DEFINE(UM_PROT_EXEC, PROT_EXEC);
}
