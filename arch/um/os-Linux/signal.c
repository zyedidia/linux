// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Anton Ivanov (aivanov@{brocade.com,kot-begemot.co.uk})
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2004 PathScale, Inc
 * Copyright (C) 2004 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <strings.h>
#include <arch.h>
#include <as-layout.h>
#include <kern_util.h>
#include <os.h>
#include <skas.h>
#include <sysdep/mcontext.h>
#include <um_malloc.h>
#include <sys/syscall.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <smp.h>
#include <timetravel.h>
#include "internal.h"

void (*sig_info[NSIG])(int, struct siginfo *, struct uml_pt_regs *, void *mc) = {
	[SIGTRAP]	= relay_signal,
	[SIGFPE]	= relay_signal,
	[SIGILL]	= relay_signal,
	[SIGWINCH]	= winch,
	[SIGBUS]	= relay_signal,
	[SIGSEGV]	= segv_handler,
	[SIGIO]		= sigio_handler,
	[SIGCHLD]	= sigchld_handler,
};

static void sig_handler_common(int sig, struct siginfo *si, mcontext_t *mc)
{
	struct uml_pt_regs r;
	unsigned long fixup;

	r.is_user = 0;
	if (sig == SIGSEGV || sig == SIGBUS) {
		/* For faults, we want the data from the sigcontext. */
		get_regs_from_mc(&r, mc);
		if (sig == SIGSEGV)
			GET_FAULTINFO_FROM_MC(r.faultinfo, mc);
	}

	/*
	 * A bus error in an I/O memory accessor continues after the access.
	 * This is done before signals are enabled, since the accessor may run
	 * with interrupts disabled.
	 */
	if (IS_ENABLED(CONFIG_INDIRECT_IOMEM_FALLBACK) && sig == SIGBUS) {
		fixup = uml_iomem_fixup(UPT_IP(&r));
		if (fixup) {
			mc_set_rip(mc, (void *)fixup);
			return;
		}
	}

	/* enable signals if sig isn't IRQ signal */
	if ((sig != SIGIO) && (sig != SIGWINCH) && (sig != SIGCHLD))
		unblock_signals_trace();

	(*sig_info[sig])(sig, si, &r, mc);
}

/*
 * These are the asynchronous signals.  SIGPROF is excluded because we want to
 * be able to profile all of UML, not just the non-critical sections.  If
 * profiling is not thread-safe, then that is not my problem.  We can disable
 * profiling when SMP is enabled in that case.
 */
#define SIGIO_BIT 0
#define SIGIO_MASK (1 << SIGIO_BIT)

#define SIGALRM_BIT 1
#define SIGALRM_MASK (1 << SIGALRM_BIT)

#define SIGCHLD_BIT 2
#define SIGCHLD_MASK (1 << SIGCHLD_BIT)

#define IPI_BIT 3
#define IPI_MASK (1 << IPI_BIT)

__thread int signals_enabled;
#if IS_ENABLED(CONFIG_UML_TIME_TRAVEL_SUPPORT)
static int signals_blocked, signals_blocked_pending;
#endif
static __thread unsigned int signals_pending;
static __thread unsigned int signals_active;

/*
 * The IPI signal can interrupt the other signal handlers, so each one marks
 * its signal pending with a single atomic instruction.
 */
static void set_pending(unsigned int mask)
{
	__atomic_fetch_or(&signals_pending, mask, __ATOMIC_RELAXED);
}

static void sig_handler(int sig, struct siginfo *si, mcontext_t *mc)
{
	int enabled = signals_enabled;

#if IS_ENABLED(CONFIG_UML_TIME_TRAVEL_SUPPORT)
	if ((signals_blocked ||
	     __atomic_load_n(&signals_blocked_pending, __ATOMIC_SEQ_CST)) &&
	    (sig == SIGIO)) {
		/* increment so unblock will do another round */
		__atomic_add_fetch(&signals_blocked_pending, 1,
				   __ATOMIC_SEQ_CST);
		return;
	}
#endif

	if (!enabled && (sig == SIGIO)) {
		/*
		 * In TT_MODE_EXTERNAL, need to still call time-travel
		 * handlers. This will mark signals_pending by itself
		 * (only if necessary.)
		 * Note we won't get here if signals are hard-blocked
		 * (which is handled above), in that case the hard-
		 * unblock will handle things.
		 */
		if (time_travel_mode == TT_MODE_EXTERNAL)
			sigio_run_timetravel_handlers();
		else
			set_pending(SIGIO_MASK);
		return;
	}

	if (!enabled && (sig == SIGCHLD)) {
		set_pending(SIGCHLD_MASK);
		return;
	}

	block_signals_trace();

	sig_handler_common(sig, si, mc);

	um_set_signals_trace(enabled);
}

static void timer_real_alarm_handler(mcontext_t *mc)
{
	struct uml_pt_regs regs;

	if (mc != NULL)
		get_regs_from_mc(&regs, mc);
	else
		memset(&regs, 0, sizeof(regs));
	timer_handler(SIGALRM, NULL, &regs);
}

static void timer_alarm_handler(int sig, struct siginfo *unused_si, mcontext_t *mc)
{
	int enabled;

	enabled = signals_enabled;
	if (!signals_enabled) {
		set_pending(SIGALRM_MASK);
		return;
	}

	block_signals_trace();

	signals_active |= SIGALRM_MASK;

	timer_real_alarm_handler(mc);

	signals_active &= ~SIGALRM_MASK;

	um_set_signals_trace(enabled);
}

void deliver_alarm(void) {
    timer_alarm_handler(SIGALRM, NULL, NULL);
}

void timer_set_signal_handler(void)
{
	set_handler(SIGALRM);
}

int timer_alarm_pending(void)
{
	return !!(signals_pending & SIGALRM_MASK);
}

int sigio_pending(void)
{
	return !!(signals_pending & SIGIO_MASK);
}

int ipi_pending(void)
{
	return !!(signals_pending & IPI_MASK);
}

#if IS_ENABLED(CONFIG_SMP)
/* The vectors of the IPIs that arrived while signals were disabled. */
static __thread unsigned int ipis_pending;

/*
 * Disabling signals does not block the IPI signal. An IPI that arrives while
 * signals are disabled is recorded, and unblock_signals() runs it.
 */
void handle_ipi_signal(int vector)
{
	if (!signals_enabled) {
		ipis_pending |= 1 << vector;
		set_pending(IPI_MASK);
		return;
	}

	signals_enabled = 0;
	um_trace_signals_off();

	uml_ipi_handler(vector);

	um_trace_signals_on();
	signals_enabled = 1;
}

static void run_pending_ipis(void)
{
	/*
	 * A single atomic instruction, so that an IPI signal that arrives
	 * meanwhile is either taken here or recorded for the next round.
	 */
	unsigned int pending = __atomic_exchange_n(&ipis_pending, 0,
						   __ATOMIC_RELAXED);

	while (pending) {
		uml_ipi_handler(__builtin_ctz(pending));
		pending &= pending - 1;
	}
}
#else
static void run_pending_ipis(void)
{
}
#endif

void set_sigstack(void *sig_stack, int size)
{
	stack_t stack = {
		.ss_flags = 0,
		.ss_sp = sig_stack,
		.ss_size = size
	};

	if (sigaltstack(&stack, NULL) != 0)
		panic("enabling signal stack failed, errno = %d\n", errno);
}

static void sigusr1_handler(int sig, struct siginfo *unused_si, mcontext_t *mc)
{
	uml_pm_wake();
}

void register_pm_wake_signal(void)
{
	set_handler(SIGUSR1);
}

static void (*handlers[_NSIG])(int sig, struct siginfo *si, mcontext_t *mc) = {
	[SIGSEGV] = sig_handler,
	[SIGBUS] = sig_handler,
	[SIGILL] = sig_handler,
	[SIGFPE] = sig_handler,
	[SIGTRAP] = sig_handler,

	[SIGIO] = sig_handler,
	[SIGWINCH] = sig_handler,
	/* SIGCHLD is only actually registered in seccomp mode. */
	[SIGCHLD] = sig_handler,
	[SIGALRM] = timer_alarm_handler,

	[SIGUSR1] = sigusr1_handler,
};

static void hard_handler(int sig, siginfo_t *si, void *p)
{
	ucontext_t *uc = p;
	mcontext_t *mc = &uc->uc_mcontext;
	int save_errno = errno;

	(*handlers[sig])(sig, (struct siginfo *)si, mc);

	errno = save_errno;
}

void set_handler(int sig)
{
	struct sigaction action;
	int flags = SA_SIGINFO | SA_ONSTACK;
	sigset_t sig_mask;

	action.sa_sigaction = hard_handler;

	/* block irq ones */
	sigemptyset(&action.sa_mask);
	sigaddset(&action.sa_mask, SIGIO);
	sigaddset(&action.sa_mask, SIGWINCH);
	sigaddset(&action.sa_mask, SIGALRM);
	if (using_seccomp)
		sigaddset(&action.sa_mask, SIGCHLD);

	if (sig == SIGSEGV)
		flags |= SA_NODEFER;

	if (sigismember(&action.sa_mask, sig))
		flags |= SA_RESTART; /* if it's an irq signal */

	action.sa_flags = flags;
	action.sa_restorer = NULL;
	if (sigaction(sig, &action, NULL) < 0)
		panic("sigaction failed - errno = %d\n", errno);

	sigemptyset(&sig_mask);
	sigaddset(&sig_mask, sig);
	if (sigprocmask(SIG_UNBLOCK, &sig_mask, NULL) < 0)
		panic("sigprocmask failed - errno = %d\n", errno);
}

/* Signal every CPU, so that each one reads its own IRQ epoll set. */
void send_sigio_to_self(void)
{
	int cpu;

	for (cpu = 0; cpu < uml_ncpus; cpu++)
		os_send_sigio(cpu);
}

void os_send_sigio(int cpu)
{
	syscall(__NR_tgkill, os_getpid(), os_cpu_tid(cpu), SIGIO);
}

int change_sig(int signal, int on)
{
	sigset_t sigset;

	sigemptyset(&sigset);
	sigaddset(&sigset, signal);
	if (sigprocmask(on ? SIG_UNBLOCK : SIG_BLOCK, &sigset, NULL) < 0)
		return -errno;

	return 0;
}

static inline void __block_signals(void)
{
	if (!signals_enabled)
		return;

	barrier();
	signals_enabled = 0;
}

static inline void __unblock_signals(void)
{
	if (signals_enabled)
		return;

	signals_enabled = 1;
	barrier();
}

void block_signals(void)
{
	__block_signals();
	/*
	 * This must return with signals disabled, so this barrier
	 * ensures that writes are flushed out before the return.
	 * This might matter if gcc figures out how to inline this and
	 * decides to shuffle this code into the caller.
	 */
	barrier();
}

void unblock_signals(void)
{
	int save_pending;

	if (signals_enabled == 1)
		return;

	__unblock_signals();

#if IS_ENABLED(CONFIG_UML_TIME_TRAVEL_SUPPORT)
	deliver_time_travel_irqs();
#endif

	/*
	 * We loop because the IRQ handler returns with interrupts off.  So,
	 * interrupts may have arrived and we need to re-enable them and
	 * recheck signals_pending.
	 */
	while (1) {
		/*
		 * Save and reset save_pending after enabling signals.  This
		 * way, signals_pending won't be changed while we're reading it.
		 *
		 * Setting signals_enabled and reading signals_pending must
		 * happen in this order, so have the barrier here.
		 */
		barrier();

		save_pending = signals_pending;
		if (save_pending == 0)
			return;

		signals_pending = 0;

		/*
		 * We have pending interrupts, so disable signals, as the
		 * handlers expect them off when they are called.  They will
		 * be enabled again above. We need to trace this, as we're
		 * expected to be enabling interrupts already, but any more
		 * tracing that happens inside the handlers we call for the
		 * pending signals will mess up the tracing state.
		 */
		__block_signals();
		um_trace_signals_off();

		/*
		 * Deal with SIGIO first because the alarm handler might
		 * schedule, leaving the pending SIGIO stranded until we come
		 * back here.
		 *
		 * SIGIO's handler doesn't use siginfo or mcontext,
		 * so they can be NULL.
		 */
		if (save_pending & SIGIO_MASK)
			sig_handler_common(SIGIO, NULL, NULL);

		if (save_pending & IPI_MASK)
			run_pending_ipis();

		if (save_pending & SIGCHLD_MASK) {
			struct uml_pt_regs regs = {};

			sigchld_handler(SIGCHLD, NULL, &regs, NULL);
		}

		/* Do not reenter the handler */

		if ((save_pending & SIGALRM_MASK) && (!(signals_active & SIGALRM_MASK)))
			timer_real_alarm_handler(NULL);

		/*
		 * Rerun the loop only if there is still pending SIGIO or an
		 * IPI and not in TIMER handler.
		 */
		if (!(signals_pending & (SIGIO_MASK | IPI_MASK)) &&
		    (signals_active & SIGALRM_MASK))
			return;

		/* Re-enable signals and trace that we're doing so. */
		um_trace_signals_on();
		__unblock_signals();
	}
}

int um_get_signals(void)
{
	return signals_enabled;
}

int um_set_signals(int enable)
{
	int ret;
	if (signals_enabled == enable)
		return enable;

	ret = signals_enabled;
	if (enable)
		unblock_signals();
	else block_signals();

	return ret;
}

int um_set_signals_trace(int enable)
{
	int ret;
	if (signals_enabled == enable)
		return enable;

	ret = signals_enabled;
	if (enable)
		unblock_signals_trace();
	else
		block_signals_trace();

	return ret;
}

void mark_sigio_pending(void)
{
	set_pending(SIGIO_MASK);
}

#if IS_ENABLED(CONFIG_UML_TIME_TRAVEL_SUPPORT)
void block_signals_hard(void)
{
	signals_blocked++;
	barrier();
}

void unblock_signals_hard(void)
{
	static bool unblocking;

	if (!signals_blocked)
		panic("unblocking signals while not blocked");

	if (--signals_blocked)
		return;
	/*
	 * Must be set to 0 before we check pending so the
	 * SIGIO handler will run as normal unless we're still
	 * going to process signals_blocked_pending.
	 */
	barrier();

	/*
	 * Note that block_signals_hard()/unblock_signals_hard() can be called
	 * within the unblock_signals()/sigio_run_timetravel_handlers() below.
	 * This would still be prone to race conditions since it's actually a
	 * call _within_ e.g. vu_req_read_message(), where we observed this
	 * issue, which loops. Thus, if the inner call handles the recorded
	 * pending signals, we can get out of the inner call with the real
	 * signal hander no longer blocked, and still have a race. Thus don't
	 * handle unblocking in the inner call, if it happens, but only in
	 * the outermost call - 'unblocking' serves as an ownership for the
	 * signals_blocked_pending decrement.
	 */
	if (unblocking)
		return;
	unblocking = true;

	while (__atomic_load_n(&signals_blocked_pending, __ATOMIC_SEQ_CST)) {
		if (signals_enabled) {
			/* signals are enabled so we can touch this */
			signals_pending |= SIGIO_MASK;
			/*
			 * this is a bit inefficient, but that's
			 * not really important
			 */
			block_signals();
			unblock_signals();
		} else {
			/*
			 * we need to run time-travel handlers even
			 * if not enabled
			 */
			sigio_run_timetravel_handlers();
		}

		/*
		 * The decrement of signals_blocked_pending must be atomic so
		 * that the signal handler will either happen before or after
		 * the decrement, not during a read-modify-write:
		 *  - If it happens before, it can increment it and we'll
		 *    decrement it and do another round in the loop.
		 *  - If it happens after it'll see 0 for both signals_blocked
		 *    and signals_blocked_pending and thus run the handler as
		 *    usual (subject to signals_enabled, but that's unrelated.)
		 *
		 * Note that a call to unblock_signals_hard() within the calls
		 * to unblock_signals() or sigio_run_timetravel_handlers() above
		 * will do nothing due to the 'unblocking' state, so this cannot
		 * underflow as the only one decrementing will be the outermost
		 * one.
		 */
		if (__atomic_sub_fetch(&signals_blocked_pending, 1,
				       __ATOMIC_SEQ_CST) < 0)
			panic("signals_blocked_pending underflow");
	}

	unblocking = false;
}
#endif
