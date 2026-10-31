// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2017 - Cambridge Greys Ltd
 * Copyright (C) 2011 - 2014 Cisco Systems Inc
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <stdlib.h>
#include <errno.h>
#include <sys/epoll.h>
#include <signal.h>
#include <string.h>
#include <irq_user.h>
#include <os.h>
#include <unistd.h>
#include <um_malloc.h>
#include <smp.h>
#include "internal.h"

/* Epoll support */

/*
 * Each CPU has an IRQ epoll set of its own, which holds the fds whose IRQs it
 * handles, and its SIGIO handler reads only that set.
 */
static int epollfds[CONFIG_NR_CPUS] = { [0 ... CONFIG_NR_CPUS - 1] = -1 };

static __thread struct epoll_event epoll_events[UM_EPOLL_BATCH];

/* The CPU that handles each fd's IRQs, which the SIGIO helper signals. */
#define MAX_IRQ_FDS 4096
static int fd_cpus[MAX_IRQ_FDS];

/* The host thread of each CPU, which its SIGIO is directed at. */
static int cpu_tids[CONFIG_NR_CPUS];

void os_set_cpu_tid(int cpu, int tid)
{
	cpu_tids[cpu] = tid;
}

/* The main thread runs CPU 0, and its thread id is the process id. */
int os_cpu_tid(int cpu)
{
	return cpu ? cpu_tids[cpu] : getpid();
}

int os_fd_cpu(int fd)
{
	if (fd < 0 || fd >= MAX_IRQ_FDS)
		return 0;
	return __atomic_load_n(&fd_cpus[fd], __ATOMIC_RELAXED);
}

/* Helper to return an Epoll data pointer from an epoll event structure.
 * We need to keep this one on the userspace side to keep includes separate
 */

void *os_epoll_get_data_pointer(int index)
{
	return epoll_events[index].data.ptr;
}

/*
 * Helper to return the events of an epoll event structure, which the
 * kernel side compares with masks from os_event_mask().
 */
int os_epoll_get_events(int index)
{
	return epoll_events[index].events;
}
/* Helper to set the event mask.
 * The event mask is opaque to the kernel side, because it does not have
 * access to the right includes/defines for EPOLL constants.
 */

int os_event_mask(enum um_irq_type irq_type)
{
	if (irq_type == IRQ_READ)
		return EPOLLIN | EPOLLPRI | EPOLLERR | EPOLLHUP | EPOLLRDHUP;
	if (irq_type == IRQ_WRITE)
		return EPOLLOUT;
	return 0;
}

/*
 * Initial Epoll Setup
 */
int os_setup_epoll(void)
{
	int cpu;

	for (cpu = 0; cpu < uml_ncpus; cpu++) {
		epollfds[cpu] = epoll_create(UM_EPOLL_BATCH);
		if (epollfds[cpu] < 0)
			return -errno;
	}
	return 0;
}

/* The IRQ epoll set of the calling CPU. */
int os_epoll_fd(void)
{
	return epollfds[uml_curr_cpu()];
}

/*
 * Helper to run the actual epoll_wait. It and the helpers that change the
 * epoll sets run in interrupt context or under irq_lock, and strerror()
 * allocates memory, so they print errors as numbers.
 */
int os_waiting_for_events_epoll(void)
{
	int n, err;

	n = epoll_wait(os_epoll_fd(),
		(struct epoll_event *) &epoll_events, UM_EPOLL_BATCH, 0);
	if (n < 0) {
		err = -errno;
		if (errno != EINTR)
			printk(UM_KERN_ERR "os_waiting_for_events: epoll returned %d, errno %d\n",
			       n, errno);
		return err;
	}
	return n;
}


/*
 * Helper to add a fd to the epoll set of the CPU that handles it
 */
int os_add_epoll_fd(int cpu, int events, int fd, void *data)
{
	struct epoll_event event;
	int result;

	if (fd >= 0 && fd < MAX_IRQ_FDS)
		__atomic_store_n(&fd_cpus[fd], cpu, __ATOMIC_RELAXED);

	event.data.ptr = data;
	event.events = events | EPOLLET;
	result = epoll_ctl(epollfds[cpu], EPOLL_CTL_ADD, fd, &event);
	if ((result) && (errno == EEXIST))
		result = os_mod_epoll_fd(cpu, events, fd, data);
	if (result)
		printk("epollctl add err fd %d, errno %d\n", fd, errno);
	return result;
}

/*
 * Helper to mod the fd event mask and/or data backreference
 */
int os_mod_epoll_fd(int cpu, int events, int fd, void *data)
{
	struct epoll_event event;
	int result;

	event.data.ptr = data;
	event.events = events | EPOLLET;
	result = epoll_ctl(epollfds[cpu], EPOLL_CTL_MOD, fd, &event);
	if (result)
		printk(UM_KERN_ERR "epollctl mod err fd %d, errno %d\n", fd, errno);
	return result;
}

/*
 * Helper to delete the epoll fd
 */
int os_del_epoll_fd(int cpu, int fd)
{
	struct epoll_event event;
	/* This is quiet as we use this as IO ON/OFF - so it is often
	 * invoked on a non-existent fd
	 */
	return epoll_ctl(epollfds[cpu], EPOLL_CTL_DEL, fd, &event);
}

void os_set_ioignore(void)
{
	signal(SIGIO, SIG_IGN);
}

void os_close_epoll_fd(void)
{
	int cpu;

	/* Needed so we do not leak an fd when rebooting */
	for (cpu = 0; cpu < uml_ncpus; cpu++)
		os_close_file(epollfds[cpu]);
}
