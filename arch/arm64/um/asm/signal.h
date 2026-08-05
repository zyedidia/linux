/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_SIGNAL_H
#define __UM_ARM64_SIGNAL_H

/*
 * Shadows arch/arm64/include/asm/signal.h, which drags in asm/memory.h for
 * the tagged-address untagging hooks.  UML only needs the uapi definitions;
 * the generic fallbacks cover the hooks.
 */
#include <uapi/asm/signal.h>
#include <uapi/asm/siginfo.h>

#endif
