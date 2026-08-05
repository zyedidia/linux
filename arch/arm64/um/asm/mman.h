/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_MMAN_H
#define __UM_ARM64_MMAN_H

/*
 * Shadows arch/arm64/include/asm/mman.h, whose BTI/MTE prot handling is tied
 * to the arm64 cpufeature code.  UML wants the plain generic definitions.
 */
#include <uapi/asm-generic/mman.h>

#endif
