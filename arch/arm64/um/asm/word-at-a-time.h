/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_WORD_AT_A_TIME_H
#define __UM_ARM64_WORD_AT_A_TIME_H

/*
 * The native header's load_unaligned_zeropad() is built on MTE tco toggling
 * and kernel exception fixups; use the plain generic version.
 */
#include <asm-generic/word-at-a-time.h>

#endif
