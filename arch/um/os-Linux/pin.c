// SPDX-License-Identifier: GPL-2.0
/*
 * Pinning of the host threads that run UML to host CPUs.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <init.h>
#include <os.h>
#include "internal.h"

/* Affinity of the process when UML started, for threads that are not pinned. */
static cpu_set_t initial_cpus;
static int initial_cpus_valid;

/* Host CPU of each guest CPU, or -1 if the guest CPU is not pinned. */
static int cpu_pin[CONFIG_NR_CPUS] = { [0 ... CONFIG_NR_CPUS - 1] = -1 };

static cpu_set_t helper_cpus;
static int helper_cpus_valid;

/* Union of the host CPUs of the guest CPUs, for guest processes. */
static cpu_set_t userspace_cpus;
static int userspace_cpus_valid;

/*
 * The kernel's sched_getaffinity() shadows the libc one in the UML binary, so
 * this uses the pthread interface.
 */
static void save_initial_cpus(void)
{
	if (initial_cpus_valid)
		return;
	if (!pthread_getaffinity_np(pthread_self(), sizeof(initial_cpus),
				    &initial_cpus))
		initial_cpus_valid = 1;
}

/*
 * Parse a list of CPUs such as "1,3-5" into set. Return 0, or -1 if the list
 * is malformed.
 */
static int parse_cpulist(const char *str, cpu_set_t *set)
{
	unsigned long first, last;
	char *end;

	CPU_ZERO(set);
	while (*str) {
		first = strtoul(str, &end, 10);
		if (end == str)
			return -1;
		last = first;
		if (*end == '-') {
			str = end + 1;
			last = strtoul(str, &end, 10);
			if (end == str || last < first)
				return -1;
		}
		if (last >= CPU_SETSIZE)
			return -1;
		for (; first <= last; first++)
			CPU_SET(first, set);
		if (*end == ',')
			end++;
		else if (*end)
			return -1;
		str = end;
	}

	return CPU_COUNT(set) ? 0 : -1;
}

static void set_thread_cpus(int cpu, const cpu_set_t *set)
{
	int err;

	err = pthread_setaffinity_np(pthread_self(), sizeof(*set), set);
	if (err)
		os_warn("Failed to set the host CPUs of guest CPU %d, error %d\n",
			cpu, err);
}

/* Pin the calling thread, which runs guest CPU cpu, to its host CPU. */
void os_pin_cpu_thread(int cpu)
{
	cpu_set_t set;

	if (cpu_pin[cpu] < 0) {
		/* Threads inherit the pinning of guest CPU 0, so undo it. */
		if (initial_cpus_valid) {
			set_thread_cpus(cpu, &initial_cpus);
			CPU_OR(&userspace_cpus, &userspace_cpus, &initial_cpus);
		}
		return;
	}

	CPU_ZERO(&set);
	CPU_SET(cpu_pin[cpu], &set);
	set_thread_cpus(cpu, &set);
	CPU_SET(cpu_pin[cpu], &userspace_cpus);
	userspace_cpus_valid = 1;
}

/*
 * Let the host process of a guest address space run on the host CPU of every
 * guest CPU, since the guest may schedule its tasks on any of them. The raw
 * system call avoids the kernel's sched_setaffinity().
 */
void os_pin_userspace(int pid)
{
	if (!userspace_cpus_valid)
		return;
	if (syscall(__NR_sched_setaffinity, pid, sizeof(userspace_cpus),
		    &userspace_cpus))
		os_warn("Failed to set the host CPUs of process %d, errno %d\n",
			pid, errno);
}

/*
 * Create a helper thread, on the host CPUs given with helper_pin= or else on
 * those of the calling thread.
 */
int os_create_helper_thread(pthread_t *thread, void *(*routine)(void *),
			    void *arg)
{
	pthread_attr_t attr;
	int err;

	if (!helper_cpus_valid)
		return pthread_create(thread, NULL, routine, arg);

	err = pthread_attr_init(&attr);
	if (err)
		return err;
	err = pthread_attr_setaffinity_np(&attr, sizeof(helper_cpus),
					  &helper_cpus);
	if (!err)
		err = pthread_create(thread, &attr, routine, arg);
	pthread_attr_destroy(&attr);
	return err;
}

static int __init cpu_pin_setup(char *str, int *add)
{
	int pins[CONFIG_NR_CPUS];
	unsigned long host_cpu;
	char *end;
	int cpu, n;

	save_initial_cpus();

	for (n = 0; *str && n < CONFIG_NR_CPUS; n++) {
		host_cpu = strtoul(str, &end, 10);
		if (end == str || host_cpu >= CPU_SETSIZE ||
		    (*end && *end != ',')) {
			os_warn("Invalid cpu_pin= list\n");
			return 1;
		}
		pins[n] = host_cpu;
		str = *end ? end + 1 : end;
	}

	if (*str)
		os_warn("cpu_pin= lists more than %d CPUs\n", CONFIG_NR_CPUS);

	for (cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
		cpu_pin[cpu] = cpu < n ? pins[cpu] : -1;

	/*
	 * The main thread runs guest CPU 0. Pinning it now makes the threads
	 * and processes that it creates start on the same host CPU.
	 */
	os_pin_cpu_thread(0);
	return 0;
}

__uml_setup("cpu_pin=", cpu_pin_setup,
"cpu_pin=<host CPU>[,<host CPU>...]\n"
"    Pin each guest CPU to a host CPU, in order, starting with CPU 0. Helper\n"
"    threads and guest processes start on the host CPU of guest CPU 0 unless\n"
"    helper_pin= is given. Guest CPUs without an entry are not pinned.\n\n"
);

static int __init helper_pin_setup(char *str, int *add)
{
	if (parse_cpulist(str, &helper_cpus)) {
		os_warn("Invalid helper_pin= list\n");
		return 1;
	}

	/* Helper threads cannot be created with only CPUs UML may not use. */
	save_initial_cpus();
	if (initial_cpus_valid) {
		CPU_AND(&helper_cpus, &helper_cpus, &initial_cpus);
		if (!CPU_COUNT(&helper_cpus)) {
			os_warn("helper_pin= has no host CPUs that UML may use\n");
			return 1;
		}
	}

	helper_cpus_valid = 1;
	return 0;
}

__uml_setup("helper_pin=", helper_pin_setup,
"helper_pin=<host CPU list>\n"
"    Run UML's helper threads, such as the SIGIO helper, on these host CPUs,\n"
"    given as a list such as 2,4-5.\n\n"
);
