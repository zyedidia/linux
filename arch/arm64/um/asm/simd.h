/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_ARM64_SIMD_H
#define __UM_ARM64_SIMD_H

/*
 * Shadows the native header, which is tied to the arm64 cpufeature code.
 * may_use_simd() == false just routes crypto through the scalar paths.
 */
#include <asm-generic/simd.h>

#endif
