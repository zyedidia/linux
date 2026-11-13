/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_SHARED_MPK_H
#define __UM_SHARED_MPK_H

/*
 * Memory protection keys split UML into two domains. D0 holds only key 0 and
 * is all of UML outside the gates. D1 also holds key 1, which tags every page
 * of the window, and runs only inside gates. The host's signal handlers start
 * with the PKRU of D0.
 */
#define UML_MPK_KEY		1
#define UML_MPK_PKRU_D0		0x55555554
#define UML_MPK_PKRU_D1		0x55555550

/* The window is aligned to its size, so that its address has one high word. */
#define UML_MPK_WINDOW_SHIFT	32
#define UML_MPK_WINDOW_SIZE	(1ULL << UML_MPK_WINDOW_SHIFT)

/*
 * Each CPU has an area that its GS base points to, and stacks for the gates
 * that run on it. A gate claims a free stack, so that a gate in a signal
 * handler does not use the stack of the gate that the signal interrupted.
 */
#define UML_MPK_STACKS		8
#define UML_MPK_STACK_SIZE	(16 << 10)

#define UML_MPK_CPU_SELF	0
#define UML_MPK_CPU_SLOTS	8
#define UML_MPK_CPU_STACK	16

#ifndef __ASSEMBLER__

extern int uml_mpk_enabled;
extern unsigned long uml_mpk_window;

static inline unsigned int uml_mpk_rdpkru(void)
{
	unsigned int eax, edx;

	asm volatile("rdpkru" : "=a" (eax), "=d" (edx) : "c" (0));
	return eax;
}

/* Return whether a range overlaps the window, counting a wrapping range. */
static inline int uml_mpk_overlaps(unsigned long addr, unsigned long len)
{
	unsigned long last = addr + len - 1;

	if (!IS_ENABLED(CONFIG_UML_MPK) || !uml_mpk_enabled || !len)
		return 0;
	if (last < addr)
		return 1;
	return addr < uml_mpk_window + UML_MPK_WINDOW_SIZE &&
	       last >= uml_mpk_window;
}

extern char __binary_start[], _end[];

/*
 * Return whether a range overlaps the window or the kernel image, which
 * holds the state outside the window that the gates trust. No mapping of
 * UML's own replaces part of either after boot.
 */
static inline int uml_mpk_protected(unsigned long addr, unsigned long len)
{
	unsigned long last = addr + len - 1;

	if (!IS_ENABLED(CONFIG_UML_MPK) || !uml_mpk_enabled || !len)
		return 0;
	if (last < addr)
		return 1;
	return uml_mpk_overlaps(addr, len) ||
	       (addr < (unsigned long)_end &&
		last >= (unsigned long)__binary_start);
}

/*
 * Functions that gates run are placed in their own section, which is the only
 * one that uml_mpk_call() calls into.
 */
#define __uml_mpk_entry \
	__attribute__((__section__(".text.uml_mpk"), __used__, __noinline__))

long uml_mpk_call(void *fn, unsigned long a0, unsigned long a1,
		  unsigned long a2, unsigned long a3, unsigned long a4,
		  unsigned long a5);
long uml_mpk_call_plain(void *fn, unsigned long a0, unsigned long a1,
			unsigned long a2, unsigned long a3, unsigned long a4,
			unsigned long a5);
long uml_mpk_syscall(long nr, unsigned long a0, unsigned long a1,
		     unsigned long a2, unsigned long a3, unsigned long a4,
		     unsigned long a5);
extern char uml_mpk_syscall_ip[];
extern char uml_mpk_gate_start[], uml_mpk_gate_end[];

void uml_mpk_gate_failed(void) __attribute__((__noreturn__));
void uml_mpk_fail(const char *msg) __attribute__((__noreturn__));
void uml_mpk_sigreturn(void *uc, unsigned long ssp)
	__attribute__((__noreturn__));

/* The shadow stack pointer, or 0 without shadow stacks. */
static inline unsigned long uml_mpk_rdssp(void)
{
	unsigned long ssp = 0;

	asm volatile("rdsspq %0" : "+r" (ssp));
	return ssp;
}

/*
 * A signal handler calls uml_mpk_signal_enter() first, with the shadow stack
 * pointer that it started with, which returns whether the signal interrupted
 * D1, and uml_mpk_signal_exit() last, which returns from the signal itself
 * under protection keys. A handler changes where the interrupted code
 * continues with uml_mpk_set_ip().
 */
void uml_mpk_init(void);
void uml_mpk_cpu_init(int cpu);
int uml_mpk_signal_enter(int sig, void *uc, unsigned long ssp);
void uml_mpk_signal_exit(void *uc);
void uml_mpk_set_ip(void *mc, unsigned long ip);

/*
 * These run only in D1. Allocations are zeroed. The map functions place a
 * mapping in the window with key 1, and return 0 or a negative error.
 */
void *uml_mpk_alloc(unsigned long size);
void uml_mpk_free(void *p);
int uml_mpk_map(int fd, unsigned long long offset, unsigned long size,
		int r, int w, void **addr);
int uml_mpk_adopt(void *from, unsigned long size, int r, int w, void **addr);
void uml_mpk_unmap(void *addr, unsigned long size);

#ifdef CONFIG_UML_MPK_TEST
int uml_mpk_test_frame(void);
int uml_mpk_test_regs(void);
void uml_mpk_test_munmap(void);
int uml_mpk_test_maps(void);
#endif

/* A spinlock for D1, whose state is all in the lock. */
struct uml_mpk_lock {
	int locked;
};

static inline void uml_mpk_lock(struct uml_mpk_lock *l)
{
	while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE))
		while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED))
			asm volatile("pause" ::: "memory");
}

static inline void uml_mpk_unlock(struct uml_mpk_lock *l)
{
	__atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

#endif /* __ASSEMBLER__ */

#endif /* __UM_SHARED_MPK_H */
