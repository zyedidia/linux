// SPDX-License-Identifier: GPL-2.0
/*
 * A seccomp filter that limits UML to the host system calls that it makes
 * when it runs from file descriptors that it was given.
 *
 * Processes inherit the filter, so it also applies to the stub processes that
 * run guest userspace. The stub's own filter traps every system call that
 * guest userspace makes, and that trap takes precedence over every action of
 * this filter other than a kill. This filter therefore never kills, and
 * traps the system calls that it denies.
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#if IS_ENABLED(CONFIG_UML_VDUSE)
#include <linux/vduse.h>
#endif
#if IS_ENABLED(CONFIG_UML_PCI_OVER_VFIO)
#include <linux/vfio.h>
#endif
#include <init.h>
#include <os.h>
#include <skas.h>

static int sandbox;

static int __init set_sandbox(char *line, int *add)
{
	*add = 0;
	sandbox = 1;
	return 0;
}

__uml_setup("sandbox", set_sandbox,
"sandbox\n"
"    Install a seccomp filter before the kernel starts that allows only the\n"
"    host system calls that UML needs to run from host file descriptors that\n"
"    it was given. Most denied system calls stop UML with a message that\n"
"    gives their number. This needs a 64-bit UML built without\n"
"    CONFIG_BLK_DEV_UBD, CONFIG_UML_RANDOM and CONFIG_PTY_CHAN, and the\n"
"    seccomp=on, memfd and no_uml_dir options, and every host resource must\n"
"    be passed as a file descriptor.\n\n"
);

#if IS_ENABLED(CONFIG_64BIT)

static const unsigned int allowed_syscalls[] = {
	__NR_futex,
	__NR_read,
	__NR_write,
	__NR_writev,
	__NR_pread64,
	__NR_pwrite64,
	__NR_sendmsg,
	__NR_recvmsg,
	__NR_sendto,
	__NR_recvfrom,
	__NR_ppoll,
	__NR_epoll_wait,
	__NR_epoll_ctl,
	__NR_epoll_create,
	__NR_eventfd2,
	__NR_signalfd4,
	__NR_close,
	__NR_close_range,
	__NR_dup2,
	__NR_fcntl,
	__NR_lseek,
	__NR_fstat,
	__NR_mmap,
	__NR_munmap,
	__NR_mprotect,
	__NR_madvise,
	__NR_memfd_create,
	__NR_rt_sigaction,
	__NR_rt_sigprocmask,
	__NR_rt_sigreturn,
	__NR_sigaltstack,
	__NR_pause,
	__NR_kill,
	__NR_tgkill,
	__NR_rt_tgsigqueueinfo,
	__NR_timer_create,
	__NR_timer_settime,
	__NR_clock_gettime,
	__NR_sched_setaffinity,
	__NR_getpid,
	__NR_gettid,
	__NR_getuid,
	__NR_getpgrp,
	__NR_uname,
	__NR_getrandom,
	__NR_arch_prctl,
	__NR_set_robust_list,
	__NR_rseq,
	__NR_setsid,
	__NR_wait4,
	__NR_execveat,
	__NR_exit,
	__NR_exit_group,
};

/*
 * The VFIO ioctls, VDUSE device ioctls and console terminal ioctls that UML
 * uses. The ioctls of the VDUSE control device are not among them.
 */
static const unsigned int allowed_ioctls[] = {
#if IS_ENABLED(CONFIG_UML_PCI_OVER_VFIO)
	VFIO_GET_API_VERSION,
	VFIO_CHECK_EXTENSION,
	VFIO_SET_IOMMU,
	VFIO_IOMMU_GET_INFO,
	VFIO_IOMMU_MAP_DMA,
	VFIO_IOMMU_UNMAP_DMA,
	VFIO_GROUP_SET_CONTAINER,
	VFIO_GROUP_UNSET_CONTAINER,
	VFIO_GROUP_GET_DEVICE_FD,
	VFIO_DEVICE_GET_INFO,
	VFIO_DEVICE_GET_REGION_INFO,
	VFIO_DEVICE_GET_IRQ_INFO,
	VFIO_DEVICE_SET_IRQS,
	VFIO_DEVICE_RESET,
#endif
#if IS_ENABLED(CONFIG_UML_VDUSE)
	VDUSE_DEV_GET_FEATURES,
	VDUSE_VQ_SETUP,
	VDUSE_VQ_GET_INFO,
	VDUSE_VQ_SETUP_KICKFD,
	VDUSE_VQ_INJECT_IRQ,
	VDUSE_IOTLB_GET_FD,
	VDUSE_IOTLB_GET_INFO,
	VDUSE_IOTLB_REG_UMEM,
	VDUSE_IOTLB_DEREG_UMEM,
#endif
	TCGETS,
	TCSETS,
	TCSETSW,
	TCSETSF,
	TIOCGWINSZ,
	TIOCGPGRP,
};

static const unsigned int allowed_prctls[] = {
	PR_SET_NAME,
	PR_SET_PDEATHSIG,
	PR_SET_NO_NEW_PRIVS,
};

#define CLONE_NAMESPACES (CLONE_NEWNS | CLONE_NEWCGROUP | CLONE_NEWUTS | \
			  CLONE_NEWIPC | CLONE_NEWUSER | CLONE_NEWPID | \
			  CLONE_NEWNET)

static struct sock_filter filter[256];
static unsigned short filter_len;

static void __init emit(unsigned short code, unsigned int k, unsigned int jt,
			unsigned int jf)
{
	if (filter_len == ARRAY_SIZE(filter) || jt > 255 || jf > 255) {
		os_warn("The sandbox filter is too large\n");
		exit(1);
	}
	filter[filter_len++] = (struct sock_filter)BPF_JUMP(code, k, jt, jf);
}

static void __init load(unsigned int offset)
{
	emit(BPF_LD | BPF_W | BPF_ABS, offset, 0, 0);
}

static void __init ret(unsigned int action)
{
	emit(BPF_RET | BPF_K, action, 0, 0);
}

/* Return action if the accumulator is k. */
static void __init ret_if(unsigned int k, unsigned int action)
{
	emit(BPF_JMP | BPF_JEQ | BPF_K, k, 0, 1);
	ret(action);
}

/*
 * Allow system call nr if the low 32 bits of argument arg are one of the n
 * values, and trap it otherwise. The accumulator holds the system call
 * number before and after this block.
 */
static void __init allow_args(unsigned int nr, int arg,
			      const unsigned int *values, unsigned int n)
{
	unsigned int i;

	emit(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 2 * n + 2);
	load(offsetof(struct seccomp_data, args[arg]));
	for (i = 0; i < n; i++)
		ret_if(values[i], SECCOMP_RET_ALLOW);
	ret(SECCOMP_RET_TRAP);
}

/*
 * Allow system call nr if the low 32 bits of its first two arguments are a0
 * and a1, and trap it otherwise.
 */
static void __init allow_arg_pair(unsigned int nr, unsigned int a0,
				  unsigned int a1)
{
	emit(BPF_JMP | BPF_JEQ | BPF_K, nr, 0, 6);
	load(offsetof(struct seccomp_data, args[0]));
	emit(BPF_JMP | BPF_JEQ | BPF_K, a0, 0, 3);
	load(offsetof(struct seccomp_data, args[1]));
	ret_if(a1, SECCOMP_RET_ALLOW);
	ret(SECCOMP_RET_TRAP);
}

static void __init build_filter(void)
{
	unsigned int i;

	load(offsetof(struct seccomp_data, arch));
	emit(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
	ret(SECCOMP_RET_TRAP);
	load(offsetof(struct seccomp_data, nr));

	for (i = 0; i < ARRAY_SIZE(allowed_syscalls); i++)
		ret_if(allowed_syscalls[i], SECCOMP_RET_ALLOW);

	/* UML kills stub processes with ptrace calls that may fail. */
	ret_if(__NR_ptrace, SECCOMP_RET_ERRNO | EPERM);
	/* A reboot executes UML again, which fails and stops it instead. */
	ret_if(__NR_execve, SECCOMP_RET_ERRNO | EPERM);
	/* The flags of clone3 are in memory, so libc falls back to clone. */
	ret_if(__NR_clone3, SECCOMP_RET_ERRNO | ENOSYS);

	emit(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 4);
	load(offsetof(struct seccomp_data, args[0]));
	emit(BPF_JMP | BPF_JSET | BPF_K, CLONE_NAMESPACES, 1, 0);
	ret(SECCOMP_RET_ALLOW);
	ret(SECCOMP_RET_TRAP);

	emit(BPF_JMP | BPF_JEQ | BPF_K, __NR_ioctl, 0,
	     2 * ARRAY_SIZE(allowed_ioctls) + 4);
	load(offsetof(struct seccomp_data, args[1]));
	for (i = 0; i < ARRAY_SIZE(allowed_ioctls); i++)
		ret_if(allowed_ioctls[i], SECCOMP_RET_ALLOW);
	/* The winch thread asks for a controlling terminal and handles failure. */
	ret_if(TIOCSCTTY, SECCOMP_RET_ERRNO | EPERM);
	ret(SECCOMP_RET_TRAP);

	allow_args(__NR_prctl, 0, allowed_prctls, ARRAY_SIZE(allowed_prctls));
	/* A datagram socket can send to any address. */
	allow_arg_pair(__NR_socketpair, AF_UNIX, SOCK_STREAM);
	/*
	 * The stub adds its filter with only TSYNC. Other flags can create a
	 * listener fd or turn off a speculation mitigation.
	 */
	allow_arg_pair(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_TSYNC);

	ret(SECCOMP_RET_TRAP);
}

/* The process that installed the filter. */
static int sandbox_pid;

static void sigsys_handler(int sig, siginfo_t *si, void *context)
{
	os_warn("The sandbox denied host system call %d at %pS\n",
		si->si_syscall, si->si_call_addr);

	/*
	 * A child that shares this handler, such as a stub before it executes,
	 * stops UML instead of signalling the process group.
	 */
	if (getpid() != sandbox_pid) {
		kill(sandbox_pid, SIGABRT);
		_exit(1);
	}
	os_dump_core();
}

void __init os_enter_sandbox(void)
{
	struct sock_fprog prog = { .filter = filter };
	struct sigaction sa = {
		.sa_sigaction = sigsys_handler,
		.sa_flags = SA_SIGINFO | SA_ONSTACK,
	};

	if (!sandbox)
		return;

	if (IS_ENABLED(CONFIG_BLK_DEV_UBD) || IS_ENABLED(CONFIG_UML_RANDOM) ||
	    IS_ENABLED(CONFIG_PTY_CHAN)) {
		os_warn("The sandbox needs a UML built without CONFIG_BLK_DEV_UBD, CONFIG_UML_RANDOM and CONFIG_PTY_CHAN, which open host paths\n");
		exit(1);
	}

	if (!using_seccomp) {
		os_warn("The sandbox needs seccomp=on\n");
		exit(1);
	}

	build_filter();
	prog.len = filter_len;
	sandbox_pid = getpid();

	sigfillset(&sa.sa_mask);
	if (sigaction(SIGSYS, &sa, NULL) < 0 ||
	    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 ||
	    syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER,
		    SECCOMP_FILTER_FLAG_TSYNC | SECCOMP_FILTER_FLAG_TSYNC_ESRCH,
		    &prog) < 0) {
		os_warn("Installing the sandbox filter failed, errno = %d\n",
			errno);
		exit(1);
	}

	os_info("Installed the sandbox filter (%u instructions)\n",
		filter_len);
}

#else

void __init os_enter_sandbox(void)
{
	if (sandbox) {
		os_warn("The sandbox needs a 64-bit UML\n");
		exit(1);
	}
}

#endif
