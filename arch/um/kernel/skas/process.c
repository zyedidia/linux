// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2002 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/sched/mm.h>

#include <asm/tlbflush.h>

#include <kern.h>
#include <os.h>
#include <skas.h>

unsigned long current_stub_stack(void)
{
	if (current->mm == NULL)
		return 0;

	return current->mm->context.id.stack;
}

struct mm_id *current_mm_id(void)
{
	if (current->mm == NULL)
		return NULL;

	return &current->mm->context.id;
}

void current_mm_sync(void)
{
	if (current->mm == NULL)
		return;

	um_tlb_sync(current->mm);
}
