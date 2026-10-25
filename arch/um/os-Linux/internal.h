/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __UM_OS_LINUX_INTERNAL_H
#define __UM_OS_LINUX_INTERNAL_H

#include <mm_id.h>
#include <stub-data.h>
#include <pthread.h>
#include <signal.h>

/*
 * irq.c
 */
int os_epoll_fd(void);
int os_fd_cpu(int fd);

/*
 * sigio.c
 */
void sigio_idle_enter(void);
int sigio_idle_exit(void);

/*
 * pin.c
 */
void os_pin_cpu_thread(int cpu);
void os_pin_userspace(int pid);
int os_create_helper_thread(pthread_t *thread, void *(*routine)(void *),
			    void *arg);

/*
 * elf_aux.c
 */
void scan_elf_aux(char **envp);

/*
 * mem.c
 */
void check_tmpexec(void);

/*
 * signal.c
 */
extern __thread int signals_enabled;
int timer_alarm_pending(void);
int sigio_pending(void);
int ipi_pending(void);
void handle_ipi_signal(int vector);

/*
 * skas/process.c
 */
void wait_stub_done(int pid);
void wait_stub_done_seccomp(struct mm_id *mm_idp, int running, int wait_sigsys);

/*
 * smp.c
 */
#define IPI_SIGNAL	SIGRTMIN

#endif /* __UM_OS_LINUX_INTERNAL_H */
