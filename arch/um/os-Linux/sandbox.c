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

#include <elf.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#if IS_ENABLED(CONFIG_UML_VDUSE)
/* The tree's header, since the host's may lack newer VDUSE ioctls. */
#include "../../../include/uapi/linux/vduse.h"
#endif
#if IS_ENABLED(CONFIG_UML_PCI_OVER_VFIO)
#include <linux/vfio.h>
#endif
#include <as-layout.h>
#include <init.h>
#include <mpk.h>
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
#if IS_ENABLED(CONFIG_UML_SHADOW_STACK)
	__NR_map_shadow_stack,
#endif
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
	VFIO_DEVICE_FEATURE,
#endif
#if IS_ENABLED(CONFIG_UML_VDUSE)
	VDUSE_DEV_GET_FEATURES,
	VDUSE_DEV_SET_CONFIG,
	VDUSE_DEV_INJECT_CONFIG_IRQ,
	VDUSE_VQ_SETUP,
	VDUSE_VQ_GET_INFO,
	VDUSE_VQ_SETUP_KICKFD,
	VDUSE_VQ_INJECT_IRQ,
	VDUSE_IOTLB_GET_FD,
	VDUSE_IOTLB_GET_INFO,
	VDUSE_IOTLB_REG_UMEM,
	VDUSE_IOTLB_DEREG_UMEM,
	VDUSE_SHM_SETUP,
	VDUSE_SHM_REG_FILE,
	VDUSE_SHM_MAP,
	VDUSE_SHM_UNMAP,
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

static struct sock_filter filter[512];
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

#if IS_ENABLED(CONFIG_UML_MPK)

#define ARG_LO(i)	offsetof(struct seccomp_data, args[i])
#define ARG_HI(i)	(offsetof(struct seccomp_data, args[i]) + 4)
#define IP_LO		offsetof(struct seccomp_data, instruction_pointer)
#define IP_HI		(IP_LO + 4)

/* Emit a jump whose targets the caller sets once it has emitted them. */
static unsigned short __init jump(unsigned short code, unsigned int k)
{
	emit(BPF_JMP | code, k, 0, 0);
	return filter_len - 1;
}

/* Return the offset from a jump to the next instruction. */
static unsigned int __init offset_to_next(unsigned short insn)
{
	unsigned int off = filter_len - insn - 1;

	if (off > 255) {
		os_warn("The sandbox filter is too large\n");
		exit(1);
	}
	return off;
}

/* Point the true or the false branch of a jump at the next instruction. */
static void __init land_jt(unsigned short insn)
{
	filter[insn].jt = offset_to_next(insn);
}

static void __init land_jf(unsigned short insn)
{
	filter[insn].jf = offset_to_next(insn);
}

static void __init stmt(unsigned short code, unsigned int k)
{
	emit(code, k, 0, 0);
}

/*
 * Allow the system call if it comes from the gates' system call stub. The
 * accumulator is clobbered.
 */
static void __init allow_from_gate(void)
{
	unsigned long ip = (unsigned long)uml_mpk_syscall_ip;

	load(IP_LO);
	emit(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)ip, 0, 3);
	load(IP_HI);
	emit(BPF_JMP | BPF_JEQ | BPF_K, ip >> 32, 0, 1);
	ret(SECCOMP_RET_ALLOW);
}

/*
 * Trap the system call unless it comes from the gates' system call stub. The
 * accumulator is clobbered.
 */
static void __init trap_unless_gate(void)
{
	unsigned long ip = (unsigned long)uml_mpk_syscall_ip;

	load(IP_LO);
	emit(BPF_JMP | BPF_JEQ | BPF_K, (unsigned int)ip, 1, 0);
	ret(SECCOMP_RET_TRAP);
	load(IP_HI);
	emit(BPF_JMP | BPF_JEQ | BPF_K, ip >> 32, 1, 0);
	ret(SECCOMP_RET_TRAP);
}

/*
 * Emit a check that the 64-bit value at offset is in [lo, hi], and return
 * the three jumps that branch out when it is not: the false branches of the
 * first two and the true branch of the last.
 */
static void __init in_range(unsigned int offset, unsigned long lo,
			    unsigned long hi, unsigned short out[3])
{
	if (lo > hi || lo >> 32 != hi >> 32) {
		os_warn("A sandbox range is empty or crosses a 4 GiB boundary\n");
		exit(1);
	}
	load(offset + 4);
	out[0] = jump(BPF_JEQ | BPF_K, lo >> 32);
	load(offset);
	out[1] = jump(BPF_JGE | BPF_K, (unsigned int)lo);
	out[2] = jump(BPF_JGT | BPF_K, (unsigned int)hi);
}

/*
 * Allow the system call if it comes from the stub code of a guest process,
 * whose address space has no window. The accumulator is clobbered.
 */
static void __init allow_from_stub(void)
{
	unsigned short out[3];

	in_range(IP_LO, STUB_START, STUB_END - 1, out);
	ret(SECCOMP_RET_ALLOW);
	land_jf(out[0]);
	land_jf(out[1]);
	land_jt(out[2]);
}

/*
 * Trap system call nr if the range in its first two arguments overlaps the
 * window, unless it comes from the gates' stub or the stub of a guest
 * process. For mmap, only a MAP_FIXED mapping is checked, since the window
 * has no hole for another one to land in. The window is aligned to its size,
 * so its high word is the same at every address in it, and the end of the
 * range is computed with a carry from the low words. The accumulator holds nr
 * before and after.
 */
static void __init trap_window(unsigned int nr, int is_mmap)
{
	unsigned int w = uml_mpk_window >> UML_MPK_WINDOW_SHIFT;
	unsigned short skip, fixed = 0, above, nocarry, carried, gt, eq, lo;

	skip = jump(BPF_JEQ | BPF_K, nr);
	allow_from_gate();
	allow_from_stub();
	if (is_mmap) {
		load(ARG_LO(3));
		fixed = jump(BPF_JSET | BPF_K, MAP_FIXED);
	}

	load(ARG_HI(0));
	above = jump(BPF_JGT | BPF_K, w);
	load(ARG_LO(0));
	stmt(BPF_ST, 0);
	load(ARG_LO(1));
	stmt(BPF_LDX | BPF_MEM, 0);
	stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
	stmt(BPF_ST, 1);
	nocarry = jump(BPF_JGE | BPF_X, 0);
	stmt(BPF_LD | BPF_IMM, 1);
	carried = filter_len;
	emit(BPF_JMP | BPF_JA, 0, 0, 0);
	land_jt(nocarry);
	stmt(BPF_LD | BPF_IMM, 0);
	filter[carried].k = filter_len - carried - 1;
	stmt(BPF_ST, 2);
	load(ARG_HI(1));
	stmt(BPF_LDX | BPF_MEM, 2);
	stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
	stmt(BPF_ST, 3);
	load(ARG_HI(0));
	stmt(BPF_LDX | BPF_MEM, 3);
	stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
	gt = jump(BPF_JGT | BPF_K, w);
	eq = jump(BPF_JEQ | BPF_K, w);
	stmt(BPF_LD | BPF_MEM, 1);
	lo = jump(BPF_JEQ | BPF_K, 0);

	land_jt(gt);
	land_jf(lo);
	ret(SECCOMP_RET_TRAP);

	land_jt(above);
	land_jf(eq);
	land_jt(lo);
	if (is_mmap)
		land_jf(fixed);
	load(offsetof(struct seccomp_data, nr));
	land_jf(skip);
}

extern char stub_exe_start[], stub_exe_end[];

/*
 * Find the executable segment of the stub executable, which a new guest
 * process runs at its link address before it jumps to the stub.
 */
static void __init stub_exe_text(unsigned long *lo, unsigned long *hi)
{
	const Elf64_Ehdr *eh = (const Elf64_Ehdr *)stub_exe_start;
	unsigned long size = stub_exe_end - stub_exe_start;
	const Elf64_Phdr *ph;
	int i, found = 0;

	if (size < sizeof(*eh) || eh->e_type != ET_EXEC ||
	    eh->e_phentsize != sizeof(*ph) || eh->e_phoff > size ||
	    eh->e_phnum > (size - eh->e_phoff) / sizeof(*ph))
		goto bad;
	ph = (const Elf64_Phdr *)(stub_exe_start + eh->e_phoff);
	for (i = 0; i < eh->e_phnum; i++) {
		if (ph[i].p_type != PT_LOAD || !(ph[i].p_flags & PF_X))
			continue;
		if (found++ || !ph[i].p_memsz)
			goto bad;
		*lo = ph[i].p_vaddr;
		*hi = ph[i].p_vaddr + ph[i].p_memsz - 1;
	}
	if (found)
		return;
bad:
	os_warn("The stub executable needs one executable segment at a fixed address\n");
	exit(1);
}

/*
 * Trap system call nr if its prot, the third argument, has PROT_EXEC, unless
 * it comes from the stub of a guest process, which maps the guest's code, or
 * from the stub executable, which maps the stub. UML maps nothing executable
 * at either in its own address space. The gates make module text executable
 * with pkey_mprotect instead. The accumulator holds nr before and after.
 */
static void __init trap_exec(unsigned int nr)
{
	unsigned short skip, noexec, stub[3], exe[3];
	unsigned long lo, hi;

	stub_exe_text(&lo, &hi);
	if ((STUB_END > uml_mpk_vm_start && STUB_START < uml_mpk_vm_end) ||
	    (hi >= uml_mpk_vm_start && lo < uml_mpk_vm_end) ||
	    uml_mpk_protected(STUB_START, STUB_SIZE) ||
	    uml_mpk_protected(lo, hi - lo + 1)) {
		os_warn("The stub overlaps the range of module text or protected memory\n");
		exit(1);
	}
	skip = jump(BPF_JEQ | BPF_K, nr);
	load(ARG_LO(2));
	noexec = jump(BPF_JSET | BPF_K, PROT_EXEC);

	/* An instruction pointer outside a range leads to the next check. */
	in_range(IP_LO, STUB_START, STUB_END - 1, stub);
	land_jf(stub[0]);
	land_jf(stub[1]);
	land_jt(stub[2]);
	in_range(IP_LO, lo, hi, exe);
	land_jf(exe[0]);
	land_jf(exe[1]);
	land_jt(exe[2]);
	ret(SECCOMP_RET_TRAP);

	land_jf(noexec);
	land_jf(stub[2]);
	land_jf(exe[2]);
	load(offsetof(struct seccomp_data, nr));
	land_jf(skip);
}

/*
 * Allow pkey_mprotect from the gates' stub with key 1 and without execute
 * permission, which protects the window, or with key 0, read and execute
 * permission and a range in the window, which seals the copy of a module's
 * text before it moves out. Trap it otherwise. The accumulator holds the
 * system call number before and after.
 */
static void __init trap_pkey_mprotect(void)
{
	unsigned int w = uml_mpk_window >> UML_MPK_WINDOW_SHIFT;
	unsigned short skip, key1, bad[4], wrap, nocarry;

	skip = jump(BPF_JEQ | BPF_K, __NR_pkey_mprotect);
	trap_unless_gate();

	load(ARG_LO(3));
	key1 = jump(BPF_JEQ | BPF_K, UML_MPK_KEY);
	load(ARG_LO(2));
	emit(BPF_JMP | BPF_JSET | BPF_K, PROT_EXEC, 0, 1);
	ret(SECCOMP_RET_TRAP);
	ret(SECCOMP_RET_ALLOW);
	land_jf(key1);

	load(ARG_LO(3));
	bad[0] = jump(BPF_JEQ | BPF_K, 0);
	load(ARG_LO(2));
	bad[1] = jump(BPF_JEQ | BPF_K, PROT_READ | PROT_EXEC);
	load(ARG_HI(0));
	bad[2] = jump(BPF_JEQ | BPF_K, w);
	load(ARG_HI(1));
	bad[3] = jump(BPF_JEQ | BPF_K, 0);

	/* The range ends in the window if the low words carry only to 0. */
	load(ARG_LO(0));
	stmt(BPF_ST, 0);
	load(ARG_LO(1));
	stmt(BPF_LDX | BPF_MEM, 0);
	stmt(BPF_ALU | BPF_ADD | BPF_X, 0);
	wrap = jump(BPF_JEQ | BPF_K, 0);
	nocarry = jump(BPF_JGE | BPF_X, 0);
	land_jf(bad[0]);
	land_jf(bad[1]);
	land_jf(bad[2]);
	land_jf(bad[3]);
	land_jf(nocarry);
	ret(SECCOMP_RET_TRAP);
	land_jt(wrap);
	land_jt(nocarry);
	ret(SECCOMP_RET_ALLOW);
	land_jf(skip);
}

/*
 * The gates alone may change the window's mappings, and pkey_mprotect,
 * mremap and VFIO DMA maps are allowed only for them. Other code may still
 * map, unmap and protect memory outside the window, but not make it
 * executable.
 */
static void __init build_mpk_rules(void)
{
	unsigned short skip;
#if IS_ENABLED(CONFIG_UML_PCI_OVER_VFIO)
	unsigned short map;
#endif

	/*
	 * An rseq area would let the host move a thread to an abort handler
	 * that D0's memory names, so threads cannot register one.
	 */
	ret_if(__NR_rseq, SECCOMP_RET_ERRNO | ENOSYS);

	trap_pkey_mprotect();

	skip = jump(BPF_JEQ | BPF_K, __NR_mremap);
	allow_from_gate();
	ret(SECCOMP_RET_TRAP);
	land_jf(skip);

#if IS_ENABLED(CONFIG_UML_PCI_OVER_VFIO)
	/* VFIO pins pages for DMA without checking their key. */
	skip = jump(BPF_JEQ | BPF_K, __NR_ioctl);
	load(ARG_LO(1));
	map = jump(BPF_JEQ | BPF_K, VFIO_IOMMU_MAP_DMA);
	allow_from_gate();
	ret(SECCOMP_RET_TRAP);
	land_jf(map);
	load(offsetof(struct seccomp_data, nr));
	land_jf(skip);
#endif

	trap_exec(__NR_mmap);
	trap_exec(__NR_mprotect);

	trap_window(__NR_munmap, 0);
	trap_window(__NR_mprotect, 0);
	trap_window(__NR_madvise, 0);
	trap_window(__NR_mmap, 1);
}

#endif

static void __init build_filter(void)
{
	unsigned int i;

	load(offsetof(struct seccomp_data, arch));
	emit(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
	ret(SECCOMP_RET_TRAP);
	load(offsetof(struct seccomp_data, nr));

#if IS_ENABLED(CONFIG_UML_MPK)
	if (uml_mpk_enabled)
		build_mpk_rules();
#endif

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
