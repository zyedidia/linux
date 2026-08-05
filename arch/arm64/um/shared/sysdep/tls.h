/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _SYSDEP_ARM64_TLS_H
#define _SYSDEP_ARM64_TLS_H

/*
 * TLS on arm64 is a plain register (tpidr_el0) with nothing to declare; this
 * header exists because the shared stub-data.h includes it unconditionally.
 */

#endif
