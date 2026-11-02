// SPDX-License-Identifier: GPL-2.0
/*
 * Host shadow stacks for UML threads.
 *
 * The host gives each of its threads one shadow stack, but UML runs many
 * threads on each host thread. Every UML thread therefore gets a shadow
 * stack of its own, which kernel_longjmp switches to along with the stack.
 */

#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <as-layout.h>
#include <os.h>

/*
 * Each entry of a shadow stack stands for at least eight bytes on the thread
 * stack or on the signal stack, so this size is enough for both.
 */
#define SHSTK_SIZE (2 * UM_THREAD_SIZE)

/*
 * The shadow stack starts with a restore token at its top. This sets ssp to
 * the address of that token, or to zero if UML runs without shadow stacks.
 */
int os_alloc_shstk(unsigned long *ssp)
{
	unsigned long cur = 0;
	long addr;

	*ssp = 0;
	asm volatile("rdsspq %0" : "+r" (cur));
	if (!cur)
		return 0;

	addr = syscall(__NR_map_shadow_stack, 0UL, (unsigned long) SHSTK_SIZE,
		       (unsigned long) SHADOW_STACK_SET_TOKEN);
	if (addr < 0)
		return -errno;

	*ssp = addr + SHSTK_SIZE - sizeof(void *);
	return 0;
}

void os_free_shstk(unsigned long ssp)
{
	if (ssp)
		munmap((void *) (ssp + sizeof(void *) - SHSTK_SIZE), SHSTK_SIZE);
}
