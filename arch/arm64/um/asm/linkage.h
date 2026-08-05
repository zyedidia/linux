/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_LINKAGE_H
#define __UM_ARM64_LINKAGE_H

/*
 * Empty on purpose: the native header pulls asm/assembler.h into every .S
 * file, which is written against kernel-mode sysreg access.  The generic
 * defaults from linux/linkage.h are all UML needs.
 */

#endif
