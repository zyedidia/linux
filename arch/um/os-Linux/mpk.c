// SPDX-License-Identifier: GPL-2.0
/*
 * Memory protection keys for UML.
 *
 * The window is a range of the address space whose pages all carry key 1. It
 * holds a control page, a pool for objects of D1, an area and gate stacks for
 * each CPU, and slots into which D1 maps memory such as the VirtIO rings. D0
 * cannot access any of it, and under the sandbox it cannot change the window's
 * mappings either.
 *
 * A signal frame lies on a stack that D0 can write, and sigreturn loads the
 * PKRU from it. A handler therefore records the frame's context when it
 * starts, and returns from the signal through a copy of the frame that D1
 * builds in the window from that record.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <cpuid.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/rseq.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <asm/prctl.h>
#include <init.h>
#include <mpk.h>
#include <os.h>

#ifndef HWCAP2_FSGSBASE
#define HWCAP2_FSGSBASE (1 << 1)
#endif

#define MPK_RSEQ_UNREGISTER	1

#define MPK_PAGE_SIZE		4096UL
#define MPK_SLOT_SHIFT		16
#define MPK_SLOT_SIZE		(1UL << MPK_SLOT_SHIFT)
#define MPK_SLOTS		(UML_MPK_WINDOW_SIZE >> MPK_SLOT_SHIFT)

#define MPK_CTL_SIZE		(256UL << 10)
#define MPK_POOL_SIZE		(64UL << 20)
#define MPK_GRANULE		64UL
#define MPK_GRANULES		(MPK_POOL_SIZE / MPK_GRANULE)

#ifdef CONFIG_SMP
#define MPK_NR_CPUS		CONFIG_NR_CPUS
#else
#define MPK_NR_CPUS		1
#endif

/*
 * Each CPU has three slots for its area and one for each stack. The area
 * starts with struct mpk_cpu, and holds a copy of a signal frame for each
 * record, from which D1 returns from the signal.
 */
#define MPK_AREA_SLOTS		3
#define MPK_CPU_SLOTS		(MPK_AREA_SLOTS + UML_MPK_STACKS)
#define MPK_COPY_OFFSET		(16UL << 10)
#define MPK_COPY_SIZE		(16UL << 10)
#define MPK_CPUS_OFFSET		(MPK_CTL_SIZE + MPK_POOL_SIZE)
#define MPK_FIRST_SLOT		((MPK_CPUS_OFFSET >> MPK_SLOT_SHIFT) + \
				 MPK_NR_CPUS * MPK_CPU_SLOTS)

#define MPK_FRAMES		8
#define MPK_AREA_SIZE	(MPK_COPY_OFFSET + MPK_FRAMES * MPK_COPY_SIZE)
#define MPK_XFEATURE_PKRU	(1UL << 9)

/*
 * The part of a ucontext that sigreturn reads, up to the kernel's signal
 * mask. The software bytes of the XSAVE area tell sigreturn which state it
 * holds, and the second magic follows the state.
 */
#define MPK_CTX_SIZE		(offsetof(ucontext_t, uc_sigmask) + 8)
#define MPK_SW_OFFSET		464
#define MPK_SW_SIZE		48
#define MPK_XSTATE_BV		512
#define MPK_XSAVE_HDR_SIZE	64

#define BITS_PER_WORD		(8 * sizeof(unsigned long))

/*
 * These are set before the kernel starts and are read-only after boot. The
 * software bytes, the second magic and the size of the XSAVE area are those
 * that the host writes into every signal frame.
 */
extern unsigned long uml_mpk_fpregs_off;
extern unsigned long uml_mpk_pkru_off;
extern unsigned char uml_mpk_sw_bytes[MPK_SW_SIZE];
extern unsigned int uml_mpk_magic2;
extern unsigned long uml_mpk_xstate_size;

struct mpk_ctl {
	struct uml_mpk_lock lock;
	unsigned long pool[MPK_GRANULES / BITS_PER_WORD];
	unsigned long slots[MPK_SLOTS / BITS_PER_WORD];
};

/*
 * A signal that a CPU took, with the frame's context as the handler found
 * it, the PKRU that the interrupted code ran with, and the shadow stack
 * pointer at the start of the handler. D1 is set when the interrupted code
 * ran in D1, and held when it is in a gate that holds a stack.
 */
struct mpk_frame {
	void *uc;
	unsigned long ssp;
	unsigned int pkru;
	int d1;
	int held;
	unsigned char ctx[MPK_CTX_SIZE] __attribute__((aligned(16)));
};

/*
 * The offsets of self, slots and stack are fixed for the gates. A signal
 * that interrupts a gate on the CPU nests its handler's records above the
 * gate's, so nframes and nheld change in an order that a nested handler
 * always finds consistent.
 */
struct mpk_cpu {
	struct mpk_cpu *self;
	unsigned long slots;
	unsigned long stack[UML_MPK_STACKS];
	long tid;
	unsigned int nframes;
	unsigned int nheld;
	struct mpk_frame frames[MPK_FRAMES];
};

_Static_assert(offsetof(struct mpk_cpu, self) == UML_MPK_CPU_SELF, "self");
_Static_assert(offsetof(struct mpk_cpu, slots) == UML_MPK_CPU_SLOTS, "slots");
_Static_assert(offsetof(struct mpk_cpu, stack) == UML_MPK_CPU_STACK, "stack");
_Static_assert(sizeof(struct mpk_cpu) <= MPK_COPY_OFFSET, "area");
_Static_assert(MPK_AREA_SIZE <= MPK_AREA_SLOTS * MPK_SLOT_SIZE, "copies");
_Static_assert(sizeof(struct mpk_ctl) <= MPK_CTL_SIZE, "ctl");
_Static_assert(UML_MPK_STACK_SIZE + MPK_PAGE_SIZE <= MPK_SLOT_SIZE, "stack");

static inline struct mpk_ctl *mpk_ctl(void)
{
	return (struct mpk_ctl *)uml_mpk_window;
}

static inline char *mpk_pool(void)
{
	return (char *)uml_mpk_window + MPK_CTL_SIZE;
}

static inline char *mpk_slot(unsigned long i)
{
	return (char *)uml_mpk_window + (i << MPK_SLOT_SHIFT);
}

static inline struct mpk_cpu *mpk_cpu_area(int cpu)
{
	return (struct mpk_cpu *)((char *)uml_mpk_window + MPK_CPUS_OFFSET +
				  cpu * MPK_CPU_SLOTS * MPK_SLOT_SIZE);
}

static inline struct mpk_cpu *mpk_cpu(void)
{
	struct mpk_cpu *c;

	asm volatile("movq %%gs:0, %0" : "=r" (c));
	return c;
}

static inline unsigned long mpk_gsbase(void)
{
	unsigned long base;

	asm volatile("rdgsbase %0" : "=r" (base));
	return base;
}

static inline void mpk_barrier(void)
{
	__atomic_signal_fence(__ATOMIC_SEQ_CST);
}

static void mpk_check_d1(void)
{
	if (uml_mpk_rdpkru() != UML_MPK_PKRU_D1)
		panic("A protected memory function ran outside a gate");
}

static inline int mpk_test(const unsigned long *map, unsigned long i)
{
	return !!(map[i / BITS_PER_WORD] & (1UL << (i % BITS_PER_WORD)));
}

static void mpk_set(unsigned long *map, unsigned long i, unsigned long n,
		    int on)
{
	for (; n; i++, n--) {
		if (on)
			map[i / BITS_PER_WORD] |= 1UL << (i % BITS_PER_WORD);
		else
			map[i / BITS_PER_WORD] &= ~(1UL << (i % BITS_PER_WORD));
	}
}

/* Find the first run of n clear bits, or return -1. */
static long mpk_find(const unsigned long *map, unsigned long nbits,
		     unsigned long n)
{
	unsigned long i, run = 0;

	for (i = 0; i < nbits; i++) {
		if (!(i % BITS_PER_WORD) && map[i / BITS_PER_WORD] == ~0UL) {
			i += BITS_PER_WORD - 1;
			run = 0;
			continue;
		}
		if (mpk_test(map, i))
			run = 0;
		else if (++run == n)
			return i + 1 - n;
	}
	return -1;
}

/* Each object is preceded by a granule that holds its number of granules. */
void *uml_mpk_alloc(unsigned long size)
{
	struct mpk_ctl *ctl = mpk_ctl();
	unsigned long n;
	char *p;
	long i;

	mpk_check_d1();
	if (!size || size > MPK_POOL_SIZE - MPK_GRANULE)
		return NULL;
	n = (size + MPK_GRANULE - 1) / MPK_GRANULE + 1;

	uml_mpk_lock(&ctl->lock);
	i = mpk_find(ctl->pool, MPK_GRANULES, n);
	if (i >= 0)
		mpk_set(ctl->pool, i, n, 1);
	uml_mpk_unlock(&ctl->lock);
	if (i < 0)
		return NULL;

	p = mpk_pool() + i * MPK_GRANULE;
	*(unsigned long *)p = n;
	memset(p + MPK_GRANULE, 0, (n - 1) * MPK_GRANULE);
	return p + MPK_GRANULE;
}

void uml_mpk_free(void *ptr)
{
	struct mpk_ctl *ctl = mpk_ctl();
	char *p = (char *)ptr - MPK_GRANULE;
	unsigned long i, n;

	mpk_check_d1();
	if (!ptr)
		return;
	if (p < mpk_pool() || p >= mpk_pool() + MPK_POOL_SIZE ||
	    (p - mpk_pool()) % MPK_GRANULE)
		uml_mpk_fail("Freeing an object that is not protected");

	i = (p - mpk_pool()) / MPK_GRANULE;
	n = *(unsigned long *)p;
	uml_mpk_lock(&ctl->lock);
	if (n < 2 || n > MPK_GRANULES - i || !mpk_test(ctl->pool, i) ||
	    !mpk_test(ctl->pool, i + n - 1))
		uml_mpk_fail("Freeing an object that is not protected");
	mpk_set(ctl->pool, i, n, 0);
	uml_mpk_unlock(&ctl->lock);
}

static long mpk_alloc_slots(unsigned long size)
{
	struct mpk_ctl *ctl = mpk_ctl();
	unsigned long n = (size + MPK_SLOT_SIZE - 1) >> MPK_SLOT_SHIFT;
	long i;

	uml_mpk_lock(&ctl->lock);
	i = mpk_find(ctl->slots, MPK_SLOTS, n);
	if (i >= 0)
		mpk_set(ctl->slots, i, n, 1);
	uml_mpk_unlock(&ctl->lock);
	return i;
}

static void mpk_free_slots(long i, unsigned long size)
{
	struct mpk_ctl *ctl = mpk_ctl();
	unsigned long n = (size + MPK_SLOT_SIZE - 1) >> MPK_SLOT_SHIFT;

	uml_mpk_lock(&ctl->lock);
	mpk_set(ctl->slots, i, n, 0);
	uml_mpk_unlock(&ctl->lock);
}

static long mpk_mmap(void *addr, unsigned long len, int prot, int flags,
		     int fd, unsigned long long offset)
{
	return uml_mpk_syscall(__NR_mmap, (unsigned long)addr, len, prot,
			       flags, fd, offset);
}

static long mpk_protect(void *addr, unsigned long len, int prot)
{
	return uml_mpk_syscall(__NR_pkey_mprotect, (unsigned long)addr, len,
			       prot, UML_MPK_KEY, 0, 0);
}

/*
 * Replace part of the window with inaccessible memory of key 1. This never
 * leaves a hole that a mapping outside the window could later fill.
 */
static void mpk_reserve(void *addr, unsigned long len)
{
	long r;

	r = mpk_mmap(addr, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS |
		     MAP_FIXED | MAP_NORESERVE, -1, 0);
	if (r >= 0)
		r = mpk_protect(addr, len, PROT_NONE);
	if (r < 0)
		uml_mpk_fail("Failed to reserve part of the protected window");
}

static int mpk_prot(int r, int w)
{
	return (r ? PROT_READ : 0) | (w ? PROT_WRITE : 0);
}

static unsigned long mpk_page_align(unsigned long size)
{
	return (size + MPK_PAGE_SIZE - 1) & ~(MPK_PAGE_SIZE - 1);
}

/*
 * Map size bytes of fd at offset, or anonymous memory if fd is negative. The
 * mapping has no access until it has key 1, so that D0 never reaches it.
 */
int uml_mpk_map(int fd, unsigned long long offset, unsigned long size,
		int r, int w, void **addr)
{
	unsigned long len = mpk_page_align(size);
	int flags = MAP_FIXED;
	long slot, err;
	char *p;

	mpk_check_d1();
	if (!size || len < size)
		return -EINVAL;
	slot = mpk_alloc_slots(len);
	if (slot < 0)
		return -ENOMEM;
	p = mpk_slot(slot);

	flags |= fd >= 0 ? MAP_SHARED : MAP_PRIVATE | MAP_ANONYMOUS;
	err = mpk_mmap(p, len, PROT_NONE, flags, fd, offset);
	if (err >= 0)
		err = mpk_protect(p, len, mpk_prot(r, w));
	if (err < 0) {
		mpk_reserve(p, len);
		mpk_free_slots(slot, len);
		return err;
	}
	*addr = p;
	return 0;
}

/* Move a mapping outside the window into it. */
int uml_mpk_adopt(void *from, unsigned long size, int r, int w, void **addr)
{
	unsigned long len = mpk_page_align(size);
	long slot, err;
	char *p;

	mpk_check_d1();
	if (!size || len < size || (unsigned long)from % MPK_PAGE_SIZE ||
	    uml_mpk_overlaps((unsigned long)from, len))
		return -EINVAL;
	if (mprotect(from, len, PROT_NONE) < 0)
		return -errno;
	slot = mpk_alloc_slots(len);
	if (slot < 0)
		return -ENOMEM;
	p = mpk_slot(slot);

	err = uml_mpk_syscall(__NR_mremap, (unsigned long)from, len, len,
			      MREMAP_MAYMOVE | MREMAP_FIXED, (unsigned long)p,
			      0);
	if (err >= 0)
		err = mpk_protect(p, len, mpk_prot(r, w));
	if (err < 0) {
		mpk_reserve(p, len);
		mpk_free_slots(slot, len);
		return err;
	}
	*addr = p;
	return 0;
}

void uml_mpk_unmap(void *addr, unsigned long size)
{
	unsigned long off = (char *)addr - (char *)uml_mpk_window;
	unsigned long len = mpk_page_align(size);

	mpk_check_d1();
	if (!size || len < size || (char *)addr < (char *)uml_mpk_window ||
	    off % MPK_SLOT_SIZE || off >> MPK_SLOT_SHIFT < MPK_FIRST_SLOT ||
	    len > UML_MPK_WINDOW_SIZE - off)
		uml_mpk_fail("Unmapping memory that is not a protected mapping");
	mpk_reserve(addr, len);
	mpk_free_slots(off >> MPK_SLOT_SHIFT, len);
}

static unsigned int *mpk_pkru_field(ucontext_t *uc)
{
	return (unsigned int *)((char *)uc + uml_mpk_fpregs_off +
				uml_mpk_pkru_off);
}

static int mpk_in_gate_code(unsigned long ip)
{
	return ip >= (unsigned long)uml_mpk_gate_start &&
	       ip < (unsigned long)uml_mpk_gate_end;
}

/* Return the index of the stack that the calling gate runs on. */
static int mpk_own_stack(struct mpk_cpu *c)
{
	unsigned long sp;
	int i;

	asm volatile("movq %%rsp, %0" : "=r" (sp));
	for (i = 0; i < UML_MPK_STACKS; i++) {
		if (sp < c->stack[i] && sp >= c->stack[i] - UML_MPK_STACK_SIZE)
			return i;
	}
	uml_mpk_fail("A protection key gate runs on another stack");
}

/*
 * A gate's stack is not the signal stack, so the kernel puts the frame of a
 * signal that interrupts a gate at the top of the signal stack. If that gate
 * runs in a handler, the frame overwrites the handler's own. The gate's
 * stack holds its caller's stack pointer at the top, which is 0 until the
 * gate stores it.
 */
static void mpk_check_nesting(struct mpk_cpu *c, const stack_t *ss)
{
	unsigned long sp, base = (unsigned long)ss->ss_sp;
	int i, own = mpk_own_stack(c);

	for (i = 0; i < UML_MPK_STACKS; i++) {
		if (i == own || !(c->slots & (1UL << i)))
			continue;
		sp = *(unsigned long *)(c->stack[i] - sizeof(long));
		if (sp >= base && sp - base < ss->ss_size)
			uml_mpk_fail("A signal arrived in a protection key gate of a signal handler");
	}
}

/*
 * Record a signal that the CPU took. The context is copied first and then
 * checked, so that a write to the frame cannot change it in between. The
 * frame's PKRU must be that of D1 only if the interrupted code is in a gate,
 * which holds a stack that no record accounts for, or in the gate code
 * before it claims one or after it gives one back. A record left by a
 * handler that did not return, such as one that jumped to a fixup, is below
 * the frame on the signal stack, since the frames of handlers that still
 * run are above it.
 */
static long __uml_mpk_entry mpk_enter(long sig, ucontext_t *uc,
				      unsigned long ssp)
{
	struct mpk_cpu *c = mpk_cpu();
	unsigned int n = c->nframes, gates;
	struct mpk_frame *f;
	ucontext_t *ctx;
	int in_gate;

	if (uml_mpk_syscall(__NR_gettid, 0, 0, 0, 0, 0, 0) != c->tid)
		uml_mpk_fail("A signal handler ran on a thread with another thread's protection key area");
	if (uml_mpk_overlaps((unsigned long)uc - sizeof(long),
			     uml_mpk_fpregs_off + uml_mpk_xstate_size +
			     sizeof(long) + sizeof(unsigned int)))
		uml_mpk_fail("A signal frame is in the protected window");

	while (n && c->frames[n - 1].uc <= (void *)uc) {
		if (c->frames[n - 1].d1)
			uml_mpk_fail("A signal handler did not return to a gate");
		n--;
	}
	if (n == MPK_FRAMES)
		uml_mpk_fail("Too many nested signals");

	f = &c->frames[n];
	memcpy(f->ctx, uc, MPK_CTX_SIZE);
	f->pkru = __atomic_load_n(mpk_pkru_field(uc), __ATOMIC_RELAXED);
	ctx = (ucontext_t *)f->ctx;

	if (!(ctx->uc_stack.ss_flags & SS_DISABLE) &&
	    uml_mpk_overlaps((unsigned long)ctx->uc_stack.ss_sp,
			     ctx->uc_stack.ss_size))
		uml_mpk_fail("A signal frame gives a signal stack in the protected window");
	mpk_check_nesting(c, &ctx->uc_stack);

	gates = __builtin_popcountl(c->slots) - 1;
	in_gate = gates > c->nheld;
	if (f->pkru == UML_MPK_PKRU_D1) {
		if (!in_gate &&
		    !mpk_in_gate_code(ctx->uc_mcontext.gregs[REG_RIP]))
			uml_mpk_fail("A signal frame gives a forged protection key state");
		if (sig == SIGILL || sig == SIGBUS || sig == SIGFPE ||
		    sig == SIGTRAP)
			uml_mpk_fail("A fault in a protection key gate");
	} else if (f->pkru != UML_MPK_PKRU_D0 || in_gate) {
		uml_mpk_fail("A signal frame gives a forged protection key state");
	}

	f->uc = uc;
	f->ssp = ssp;
	f->d1 = f->pkru == UML_MPK_PKRU_D1;
	f->held = f->d1 && in_gate;
	mpk_barrier();
	c->nframes = n + 1;
	mpk_barrier();
	if (f->held)
		c->nheld++;
	return f->d1;
}

/*
 * Change where the code that the last signal interrupted continues, for a
 * fixup. It must continue in D0 code of the kernel, outside the gates.
 */
static long __uml_mpk_entry mpk_set_ip(ucontext_t *uc, unsigned long ip)
{
	extern char _text[], _etext[];
	extern char __uml_mpk_text_start[], __uml_mpk_text_end[];
	struct mpk_cpu *c = mpk_cpu();
	unsigned int n = c->nframes;
	struct mpk_frame *f;

	if (!n || c->frames[n - 1].uc != uc)
		uml_mpk_fail("A signal handler moved a frame's instruction pointer");
	f = &c->frames[n - 1];
	if (f->d1 || ip < (unsigned long)_text || ip >= (unsigned long)_etext ||
	    mpk_in_gate_code(ip) ||
	    (ip >= (unsigned long)__uml_mpk_text_start &&
	     ip < (unsigned long)__uml_mpk_text_end))
		uml_mpk_fail("A signal handler moved a frame's instruction pointer");
	((ucontext_t *)f->ctx)->uc_mcontext.gregs[REG_RIP] = ip;
	return 0;
}

/*
 * Return from the last signal through a copy of its frame in the window,
 * built from the record and the frame's XSAVE area. Sigreturn reads the copy
 * with the PKRU of D1, so nothing that D0 writes to the frame meanwhile
 * changes the registers, the PKRU, the signal mask or the signal stack that
 * the interrupted code continues with.
 */
static long __uml_mpk_entry mpk_exit(ucontext_t *uc)
{
	struct mpk_cpu *c = mpk_cpu();
	unsigned int n = c->nframes;
	unsigned long ssp, xfeatures;
	struct mpk_frame *f;
	char *copy, *fp;
	ucontext_t *cuc;
	int own;

	if (!n || c->frames[n - 1].uc != uc)
		uml_mpk_fail("A signal handler returned through another frame");
	f = &c->frames[n - 1];

	copy = (char *)c + MPK_COPY_OFFSET + (n - 1) * MPK_COPY_SIZE;
	fp = copy + ((sizeof(long) + uml_mpk_fpregs_off + 63) & ~63UL);
	cuc = (ucontext_t *)(fp - uml_mpk_fpregs_off);
	memcpy(fp, (char *)uc + uml_mpk_fpregs_off,
	       uml_mpk_xstate_size + sizeof(unsigned int));
	memcpy(cuc, f->ctx, MPK_CTX_SIZE);
	cuc->uc_mcontext.fpregs = (void *)fp;
	/*
	 * D1 code uses no vector registers, so the vector state comes from the
	 * frame. The header after xstate_bv must be zero, since a set
	 * compaction bit would have XRSTOR take the PKRU from another offset.
	 */
	memcpy(fp + MPK_SW_OFFSET, uml_mpk_sw_bytes, MPK_SW_SIZE);
	*(unsigned int *)(fp + uml_mpk_xstate_size) = uml_mpk_magic2;
	xfeatures = *(unsigned long *)(uml_mpk_sw_bytes + 8);
	*(unsigned long *)(fp + MPK_XSTATE_BV) &= xfeatures;
	*(unsigned long *)(fp + MPK_XSTATE_BV) |= MPK_XFEATURE_PKRU;
	memset(fp + MPK_XSTATE_BV + sizeof(long), 0,
	       MPK_XSAVE_HDR_SIZE - sizeof(long));
	*(unsigned int *)(fp + uml_mpk_pkru_off) = f->pkru;
	ssp = f->ssp;

	if (f->held)
		c->nheld--;
	mpk_barrier();
	c->nframes = n - 1;

	/*
	 * This gate never returns, so it gives back its stack, with the word
	 * that holds its caller's stack pointer cleared, as the gate code does.
	 * Nothing can claim the stack before sigreturn, since the handler
	 * blocks the signals that could arrive.
	 */
	own = mpk_own_stack(c);
	*(unsigned long *)(c->stack[own] - sizeof(long)) = 0;
	__atomic_fetch_and(&c->slots, ~(1UL << own), __ATOMIC_RELEASE);
	uml_mpk_sigreturn(cuc, ssp);
}

/*
 * The host moves a thread that it interrupts in the critical section that
 * the thread's rseq area names to that section's abort handler, and the
 * area is in D0's memory. A thread that runs gates therefore has none. The
 * CPU number in the area is negative while it is not registered.
 */
static int mpk_drop_rseq(void)
{
	struct rseq *rs = (void *)((char *)__builtin_thread_pointer() +
				   __rseq_offset);

	if (!__rseq_size || (int)rs->cpu_id < 0)
		return 0;
	if (syscall(__NR_rseq, rs, __rseq_size, MPK_RSEQ_UNREGISTER,
		    RSEQ_SIG) &&
	    syscall(__NR_rseq, rs, 32, MPK_RSEQ_UNREGISTER, RSEQ_SIG))
		return -errno;
	return 0;
}

static long __uml_mpk_entry mpk_set_tid(void)
{
	mpk_cpu()->tid = uml_mpk_syscall(__NR_gettid, 0, 0, 0, 0, 0, 0);
	return 0;
}

/*
 * A thread without an area of its own, such as a helper thread or a clone,
 * never runs in D1, but its frame could still give it any PKRU at sigreturn,
 * so its signals stop UML.
 */
int uml_mpk_signal_enter(int sig, void *uc, unsigned long ssp)
{
	if (!uml_mpk_enabled)
		return 0;
	if (!mpk_gsbase())
		panic("Signal %d on a thread without a protection key area", sig);
	return uml_mpk_call(mpk_enter, sig, (unsigned long)uc, ssp, 0, 0, 0);
}

void uml_mpk_signal_exit(void *uc)
{
	if (uml_mpk_enabled)
		uml_mpk_call(mpk_exit, (unsigned long)uc, 0, 0, 0, 0, 0);
}

void uml_mpk_set_ip(void *mc, unsigned long ip)
{
	if (uml_mpk_enabled)
		uml_mpk_call(mpk_set_ip, (unsigned long)mc -
			     offsetof(ucontext_t, uc_mcontext), ip, 0, 0, 0, 0);
}

void uml_mpk_cpu_init(int cpu)
{
	if (!uml_mpk_enabled)
		return;
	if (cpu < 0 || cpu >= MPK_NR_CPUS || mpk_drop_rseq() ||
	    syscall(__NR_arch_prctl, ARCH_SET_GS,
		    (unsigned long)mpk_cpu_area(cpu)))
		panic("Failed to give CPU %d its protection key area", cpu);
	uml_mpk_call(mpk_set_tid, 0, 0, 0, 0, 0, 0);
}

static volatile long mpk_probe_off;
static volatile unsigned int mpk_probe_pkru;
static char mpk_probe_fp[MPK_COPY_SIZE] __attribute__((aligned(64)));

static void mpk_probe_handler(int sig, siginfo_t *si, void *p)
{
	ucontext_t *uc = p;
	char *fp = (char *)uc->uc_mcontext.fpregs;
	unsigned int size = *(unsigned int *)(fp + MPK_SW_OFFSET + 4);

	mpk_probe_off = fp - (char *)uc;
	mpk_probe_pkru = uml_mpk_rdpkru();
	if (size <= sizeof(mpk_probe_fp))
		memcpy(mpk_probe_fp, fp, size);
}

/*
 * Find where signal frames keep the PKRU and what their XSAVE areas say
 * about themselves, and check that handlers start without key 1.
 */
static int __init mpk_probe_frames(void)
{
	struct sigaction sa = {
		.sa_sigaction = mpk_probe_handler,
		.sa_flags = SA_SIGINFO,
	}, old;
	unsigned int magic1, size, xstate_size;
	unsigned long xfeatures;
	sigset_t set, oset;

	sigemptyset(&sa.sa_mask);
	sigemptyset(&set);
	sigaddset(&set, SIGUSR2);
	if (sigaction(SIGUSR2, &sa, &old) < 0)
		return -errno;
	sigprocmask(SIG_UNBLOCK, &set, &oset);
	raise(SIGUSR2);
	sigprocmask(SIG_SETMASK, &oset, NULL);
	sigaction(SIGUSR2, &old, NULL);

	magic1 = *(unsigned int *)(mpk_probe_fp + MPK_SW_OFFSET);
	size = *(unsigned int *)(mpk_probe_fp + MPK_SW_OFFSET + 4);
	xfeatures = *(unsigned long *)(mpk_probe_fp + MPK_SW_OFFSET + 8);
	xstate_size = *(unsigned int *)(mpk_probe_fp + MPK_SW_OFFSET + 16);
	if (mpk_probe_off <= 0 || mpk_probe_pkru != UML_MPK_PKRU_D0 ||
	    magic1 != 0x46505853 || !(xfeatures & MPK_XFEATURE_PKRU) ||
	    size != xstate_size + sizeof(unsigned int) ||
	    size > sizeof(mpk_probe_fp) ||
	    ((sizeof(long) + mpk_probe_off + 63) & ~63UL) + size >
	    MPK_COPY_SIZE ||
	    *(unsigned int *)(mpk_probe_fp + xstate_size) != 0x46505845 ||
	    *(unsigned int *)(mpk_probe_fp + uml_mpk_pkru_off) !=
	    UML_MPK_PKRU_D0)
		return -EINVAL;

	uml_mpk_fpregs_off = mpk_probe_off;
	uml_mpk_xstate_size = xstate_size;
	uml_mpk_magic2 = 0x46505845;
	memcpy(uml_mpk_sw_bytes, mpk_probe_fp + MPK_SW_OFFSET, MPK_SW_SIZE);
	return 0;
}

/*
 * Lay out the window while its memory still has key 0, then give all of it
 * key 1, after which this code cannot read it. The control page, the pool,
 * the areas and the top of each stack are accessible to D1, and the rest of
 * the window to nothing.
 */
static int __init mpk_setup_window(int key)
{
	unsigned long size = UML_MPK_WINDOW_SIZE, raw, base;
	struct mpk_ctl *ctl;
	struct mpk_cpu *c;
	int cpu, i;
	char *s;

	raw = (unsigned long)mmap(NULL, 2 * size, PROT_NONE, MAP_PRIVATE |
				  MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
	if (raw == (unsigned long)MAP_FAILED)
		return -errno;
	base = (raw + size - 1) & ~(size - 1);
	if (base > raw)
		munmap((void *)raw, base - raw);
	munmap((void *)(base + size), raw + size - base);

	if (mprotect((void *)base, MPK_CPUS_OFFSET +
		     MPK_NR_CPUS * MPK_CPU_SLOTS * MPK_SLOT_SIZE,
		     PROT_READ | PROT_WRITE) < 0)
		return -errno;

	ctl = (struct mpk_ctl *)base;
	mpk_set(ctl->slots, 0, MPK_FIRST_SLOT, 1);

	for (cpu = 0; cpu < MPK_NR_CPUS; cpu++) {
		c = (struct mpk_cpu *)(base + MPK_CPUS_OFFSET +
				       cpu * MPK_CPU_SLOTS * MPK_SLOT_SIZE);
		c->self = c;
		c->tid = -1;
		for (i = 0; i < UML_MPK_STACKS; i++) {
			s = (char *)c + (MPK_AREA_SLOTS + i) * MPK_SLOT_SIZE;
			c->stack[i] = (unsigned long)s + MPK_SLOT_SIZE;
		}
	}

	if (syscall(__NR_pkey_mprotect, base, size, PROT_NONE, key) < 0 ||
	    syscall(__NR_pkey_mprotect, base, MPK_CPUS_OFFSET,
		    PROT_READ | PROT_WRITE, key) < 0)
		return -errno;
	for (cpu = 0; cpu < MPK_NR_CPUS; cpu++) {
		c = (struct mpk_cpu *)(base + MPK_CPUS_OFFSET +
				       cpu * MPK_CPU_SLOTS * MPK_SLOT_SIZE);
		if (syscall(__NR_pkey_mprotect, c, MPK_AREA_SIZE,
			    PROT_READ | PROT_WRITE, key) < 0)
			return -errno;
		for (i = 0; i < UML_MPK_STACKS; i++) {
			s = (char *)c + (MPK_AREA_SLOTS + i + 1) * MPK_SLOT_SIZE;
			s -= UML_MPK_STACK_SIZE;
			if (syscall(__NR_pkey_mprotect, s, UML_MPK_STACK_SIZE,
				    PROT_READ | PROT_WRITE, key) < 0)
				return -errno;
		}
	}

	uml_mpk_window = base;
	return 0;
}

/* This runs on the main thread, which is CPU 0, before the sandbox. */
void __init uml_mpk_init(void)
{
	unsigned int a, b, c, d;
	int key, err;

	if (!(getauxval(AT_HWCAP2) & HWCAP2_FSGSBASE)) {
		os_warn("Running without protection keys, which need FSGSBASE\n");
		return;
	}
	__cpuid_count(7, 0, a, b, c, d);
	if (!(c & (1 << 4))) {
		os_warn("Running without protection keys, which the host does not support\n");
		return;
	}
	__cpuid_count(0xd, 9, a, b, c, d);
	if (a < sizeof(unsigned int) || !b) {
		os_warn("Running without protection keys, which the host does not save\n");
		return;
	}
	uml_mpk_pkru_off = b;

	key = syscall(__NR_pkey_alloc, 0, PKEY_DISABLE_ACCESS);
	if (key != UML_MPK_KEY || uml_mpk_rdpkru() != UML_MPK_PKRU_D0) {
		os_warn("Running without protection keys, since key %d is not the first free one or the PKRU is 0x%x\n",
			key, uml_mpk_rdpkru());
		if (key >= 0)
			syscall(__NR_pkey_free, key);
		return;
	}

	err = mpk_drop_rseq();
	if (!err)
		err = mpk_probe_frames();
	if (!err)
		err = mpk_setup_window(key);
	if (err) {
		os_warn("Running without protection keys, err = %d\n", err);
		syscall(__NR_pkey_free, key);
		return;
	}

	uml_mpk_enabled = 1;
	uml_mpk_cpu_init(0);
	os_info("Protecting the VirtIO transport with protection key %d\n",
		key);
}

#ifdef CONFIG_UML_MPK_TEST

void (*uml_mpk_test_hook)(ucontext_t *uc);

static char mpk_test_fpstate[16 << 10] __attribute__((aligned(64)));

/*
 * Corrupt the frame in the ways that a write from D0 could, each of which
 * would have sigreturn load a PKRU of 0, which allows every key: the PKRU
 * itself, the software bytes, the compaction bit of the XSAVE header with a
 * PKRU at its compacted offset, and the fpregs pointer.
 */
static void mpk_test_corrupt(ucontext_t *uc)
{
	char *fp = (char *)uc->uc_mcontext.fpregs;

	uml_mpk_test_hook = NULL;
	*(unsigned int *)(fp + uml_mpk_pkru_off) = 0;
	*(unsigned int *)(fp + MPK_SW_OFFSET) = 0;
	*(unsigned long *)(fp + MPK_XSTATE_BV) |= MPK_XFEATURE_PKRU;
	*(unsigned long *)(fp + MPK_XSTATE_BV + 8) = (1UL << 63) |
						     MPK_XFEATURE_PKRU;
	*(unsigned int *)(fp + MPK_XSTATE_BV + MPK_XSAVE_HDR_SIZE) = 0;
	memcpy(mpk_test_fpstate, fp, uml_mpk_pkru_off + sizeof(unsigned int));
	uc->uc_mcontext.fpregs = (void *)mpk_test_fpstate;
}

static void mpk_test_clobber(ucontext_t *uc)
{
	uml_mpk_test_hook = NULL;
	uc->uc_mcontext.gregs[REG_RIP] = 0;
	uc->uc_mcontext.gregs[REG_RSP] = 0;
}

int uml_mpk_test_frame(void)
{
	uml_mpk_test_hook = mpk_test_corrupt;
	raise(SIGIO);
	if (uml_mpk_test_hook)
		return -EAGAIN;
	return uml_mpk_rdpkru() == UML_MPK_PKRU_D0 ? 0 : -EPERM;
}

static long __uml_mpk_entry mpk_test_signal_in_gate(void)
{
	raise(SIGIO);
	return 42;
}

int uml_mpk_test_regs(void)
{
	long r;

	uml_mpk_test_hook = mpk_test_clobber;
	r = uml_mpk_call(mpk_test_signal_in_gate, 0, 0, 0, 0, 0, 0);
	if (uml_mpk_test_hook)
		return -EAGAIN;
	return r == 42 ? 0 : -EPERM;
}

void uml_mpk_test_munmap(void)
{
	syscall(__NR_munmap, uml_mpk_window, MPK_PAGE_SIZE);
}

static long __uml_mpk_entry mpk_test_maps_d1(char *from)
{
	char *p;
	int err;

	err = uml_mpk_map(-1, 0, 3 * MPK_PAGE_SIZE, 1, 1, (void **)&p);
	if (err)
		return err;
	memset(p, 0x5a, 3 * MPK_PAGE_SIZE);
	uml_mpk_unmap(p, 3 * MPK_PAGE_SIZE);

	err = uml_mpk_adopt(from, MPK_PAGE_SIZE, 1, 1, (void **)&p);
	if (err)
		return err;
	err = p[0] == 0x3c ? 0 : -EIO;
	uml_mpk_unmap(p, MPK_PAGE_SIZE);
	return err;
}

/* Map, unmap and adopt memory in the window, as the gates do. */
int uml_mpk_test_maps(void)
{
	char *from;

	from = mmap(NULL, MPK_PAGE_SIZE, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (from == MAP_FAILED)
		return -errno;
	from[0] = 0x3c;
	return uml_mpk_call(mpk_test_maps_d1, (unsigned long)from, 0, 0, 0, 0,
			    0);
}

#endif
