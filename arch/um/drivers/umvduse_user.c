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

#include "umvduse_user.h"

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
