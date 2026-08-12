// SPDX-License-Identifier: GPL-2.0
/*
 * umvduse: host-side helpers.
 *
 * Ordinary host userspace, built with USER_CFLAGS (Makefile.rules picks
 * *_user.o up automatically), so libc is available and kernel headers are
 * not. Everything else the transport needs from the host -- open, read,
 * write, ioctl, eventfd -- already exists as os_*() helpers; only mmap
 * with an offset is missing, so that is all that lives here.
 */

#include <errno.h>
#include <sys/mman.h>
#include <sys/resource.h>

#include "umvduse_user.h"

/*
 * Raise the pinned-memory limit ourselves rather than depending on a
 * launcher to have done it.
 *
 * Raising the hard limit needs CAP_SYS_RESOURCE, so try for infinity
 * first; an unprivileged instance can still always raise the soft limit
 * up to the inherited hard one, which beats leaving it at the ~8 MiB
 * default. @effective reports what was achieved, ~0ULL meaning
 * unlimited, so the caller can say something useful about it.
 */
int umvd_user_raise_memlock(unsigned long long *effective)
{
	struct rlimit want = {
		.rlim_cur = RLIM_INFINITY,
		.rlim_max = RLIM_INFINITY,
	};
	struct rlimit lim;

	if (setrlimit(RLIMIT_MEMLOCK, &want) == 0) {
		*effective = ~0ULL;
		return 0;
	}

	if (getrlimit(RLIMIT_MEMLOCK, &lim) < 0)
		return -errno;

	if (lim.rlim_cur != lim.rlim_max) {
		lim.rlim_cur = lim.rlim_max;
		if (setrlimit(RLIMIT_MEMLOCK, &lim) < 0)
			return -errno;
	}

	*effective = lim.rlim_max == RLIM_INFINITY ? ~0ULL : lim.rlim_max;
	return 0;
}

void *umvd_user_mmap(int fd, unsigned long long offset, unsigned long len,
		     int writable)
{
	int prot = PROT_READ | (writable ? PROT_WRITE : 0);
	void *addr;

	addr = mmap(NULL, len, prot, MAP_SHARED, fd, (off_t)offset);
	if (addr == MAP_FAILED)
		return NULL;

	return addr;
}

int umvd_user_munmap(void *addr, unsigned long len)
{
	if (munmap(addr, len) < 0)
		return -errno;

	return 0;
}
