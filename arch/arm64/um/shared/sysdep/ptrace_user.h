/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SYSDEP_ARM64_PTRACE_USER_H
#define __SYSDEP_ARM64_PTRACE_USER_H

#include <generated/user_constants.h>

#define PT_OFFSET(r) ((r) * sizeof(long))

#define REGS_IP_INDEX HOST_IP
#define REGS_SP_INDEX HOST_SP

#endif
