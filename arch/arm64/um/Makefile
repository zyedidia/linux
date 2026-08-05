# SPDX-License-Identifier: GPL-2.0
#
# AArch64 host support for UML.  Kernel-only (CONFIG_UML_NO_USERSPACE);
# there is no syscall/signal-frame/stub layer for guest processes.

obj-y = bugs.o delay.o fault.o ptrace.o setjmp.o sysrq.o os-Linux/

obj-$(CONFIG_MODULES) += module.o

USER_OBJS := bugs.o fault.o

$(obj)/user-offsets.s: c_flags = -Wp,-MD,$(depfile) $(USER_CFLAGS)
targets += user-offsets.s

include/generated/user_constants.h: $(obj)/user-offsets.s FORCE
	$(call filechk,offsets,__USER_CONSTANT_H__)

include $(srctree)/arch/um/scripts/Makefile.rules
