// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Anton Ivanov (aivanov@{brocade.com,kot-begemot.co.uk})
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2012-2014 Cisco Systems
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike{addtoit,linux.intel}.com)
 */

#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/signalfd.h>
#include <sys/time.h>
#include <init.h>
#include <kern_util.h>
#include <os.h>
#include <smp.h>
#include <string.h>
#include "internal.h"

static timer_t event_high_res_timer[CONFIG_NR_CPUS] = { 0 };

static inline long long timespec_to_ns(const struct timespec *ts)
{
	return ((long long) ts->tv_sec * UM_NSEC_PER_SEC) + ts->tv_nsec;
}

long long os_persistent_clock_emulation(void)
{
	struct timespec realtime_tp;

	clock_gettime(CLOCK_REALTIME, &realtime_tp);
	return timespec_to_ns(&realtime_tp);
}

#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif

/**
 * os_timer_create() - create an new posix (interval) timer
 */
int os_timer_create(void)
{
	int cpu = uml_curr_cpu();
	timer_t *t = &event_high_res_timer[cpu];
	struct sigevent sev = {
		.sigev_notify = SIGEV_THREAD_ID,
		.sigev_signo = SIGALRM,
		.sigev_value.sival_ptr = t,
		.sigev_notify_thread_id = gettid(),
	};

	if (timer_create(CLOCK_MONOTONIC, &sev, t) == -1)
		return -1;

	return 0;
}

int os_timer_set_interval(int cpu, unsigned long long nsecs)
{
	struct itimerspec its;

	its.it_value.tv_sec = nsecs / UM_NSEC_PER_SEC;
	its.it_value.tv_nsec = nsecs % UM_NSEC_PER_SEC;

	its.it_interval.tv_sec = nsecs / UM_NSEC_PER_SEC;
	its.it_interval.tv_nsec = nsecs % UM_NSEC_PER_SEC;

	if (timer_settime(event_high_res_timer[cpu], 0, &its, NULL) == -1)
		return -errno;

	return 0;
}

int os_timer_one_shot(int cpu, unsigned long long nsecs)
{
	struct itimerspec its = {
		.it_value.tv_sec = nsecs / UM_NSEC_PER_SEC,
		.it_value.tv_nsec = nsecs % UM_NSEC_PER_SEC,

		.it_interval.tv_sec = 0,
		.it_interval.tv_nsec = 0, // we cheat here
	};

	timer_settime(event_high_res_timer[cpu], 0, &its, NULL);
	return 0;
}

/**
 * os_timer_disable() - disable the posix (interval) timer
 * @cpu: the CPU for which the timer is to be disabled
 */
void os_timer_disable(int cpu)
{
	struct itimerspec its;

	memset(&its, 0, sizeof(struct itimerspec));
	timer_settime(event_high_res_timer[cpu], 0, &its, NULL);
}

long long os_nsecs(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC,&ts);
	return timespec_to_ns(&ts);
}

static __thread int wake_signals;

void os_idle_prepare(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigaddset(&set, IPI_SIGNAL);

	/*
	 * The idle sleep keeps SIGALRM and the IPI signal blocked, and this
	 * signalfd wakes it when one of them arrives.
	 */
	wake_signals = signalfd(-1, &set, SFD_CLOEXEC);
	if (wake_signals < 0)
		panic("Failed to create signal FD, errno = %d", errno);
}

static unsigned long long idle_poll_ns;

static int __init idle_poll_setup(char *str, int *add)
{
	idle_poll_ns = strtoull(str, NULL, 0) * 1000;
	return 0;
}

__uml_setup("idle_poll=", idle_poll_setup,
"idle_poll=<usecs>\n"
"    Poll for interrupts for up to this many microseconds before the idle\n"
"    CPU that handles them sleeps, which lowers the latency of interrupts\n"
"    that arrive soon after the CPU goes idle at the cost of host CPU time.\n"
"    The default is 0.\n\n"
);

/*
 * Wait until one of the fds is ready or a signal that mask does not block
 * arrives. The IRQ epoll fd is polled only when it is the second fd.
 */
static void idle_wait(struct pollfd *fds, int n, const sigset_t *mask)
{
	struct timespec zero = {};
	long long end;

	if (idle_poll_ns && n == 2) {
		end = os_nsecs() + idle_poll_ns;
		do {
			if (ppoll(fds, n, &zero, mask) != 0 ||
			    uml_need_resched())
				return;
		} while (os_nsecs() < end);
	}

	ppoll(fds, n, NULL, mask);
}

/**
 * os_idle_sleep() - sleep until interrupted
 * @wait_irqs: whether this CPU handles SIGIO and should also wake up when an
 *	fd in the IRQ epoll set becomes ready
 *
 * Waiting on the IRQ epoll set directly wakes the CPU with one host wakeup
 * and no signal. The IRQs are then handled as a pending SIGIO when IRQs are
 * enabled again.
 */
void os_idle_sleep(int wait_irqs)
{
	struct pollfd fds[2] = {
		{ .fd = wake_signals, .events = POLLIN },
		{ .fd = os_epoll_fd(), .events = POLLIN },
	};
	int n = wait_irqs && fds[1].fd >= 0 ? 2 : 1;
	sigset_t set, old, wait;

	/*
	 * Block SIGALRM, SIGIO and the IPI signal while performing the
	 * need_resched check. The wait unblocks SIGIO again atomically, so a
	 * SIGIO that arrives after the check still interrupts it. SIGALRM and
	 * the IPI signal stay blocked because the signalfd reports them.
	 */
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	sigaddset(&set, SIGIO);
	sigaddset(&set, IPI_SIGNAL);
	sigprocmask(SIG_BLOCK, &set, &old);
	wait = old;
	sigaddset(&wait, SIGALRM);
	sigaddset(&wait, IPI_SIGNAL);

	/*
	 * Because disabling IRQs does not block these signals, it is also
	 * necessary to check for any that arrived and are pending.
	 */
	if (!uml_need_resched() && !timer_alarm_pending() && !sigio_pending() &&
	    !ipi_pending()) {
		if (n == 2)
			sigio_idle_enter();

		idle_wait(fds, n, &wait);

		/*
		 * The helper thread does not signal the CPU while it is idle,
		 * so an event that it saw has to be handled here even if the
		 * poll returned for another reason.
		 */
		if (n == 2 && (sigio_idle_exit() || (fds[1].revents & POLLIN)))
			mark_sigio_pending();
	}

	/* Restore the signal mask. */
	sigprocmask(SIG_SETMASK, &old, NULL);
}
