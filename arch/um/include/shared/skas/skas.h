/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#ifndef __SKAS_H
#define __SKAS_H

#include <sysdep/ptrace.h>

#ifdef CONFIG_UML_USERSPACE
extern int using_seccomp;
#else
/*
 * Nothing hosts userspace here, so the seccomp stub mode is simply never in
 * use.  Defining it away lets the callers that are shared with a normal build
 * fold their seccomp branches out instead of being #ifdef'd individually.
 */
#define using_seccomp 0
#endif

extern void new_thread_handler(void);
extern void handle_syscall(struct uml_pt_regs *regs);
extern unsigned long current_stub_stack(void);
extern struct mm_id *current_mm_id(void);
extern void current_mm_sync(void);
void initial_jmpbuf_lock(void);
void initial_jmpbuf_unlock(void);

#endif
