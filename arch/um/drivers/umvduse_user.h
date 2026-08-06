/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umvduse: interface between the kernel-side transport and the host-libc
 * helpers in umvduse_user.c. Plain C types only; this header is included
 * from both worlds.
 */

#ifndef __UMVDUSE_USER_H__
#define __UMVDUSE_USER_H__

void *umvd_user_mmap(int fd, unsigned long long offset, unsigned long len,
		     int writable);
int umvd_user_munmap(void *addr, unsigned long len);
int umvd_user_raise_memlock(void);

#endif /* __UMVDUSE_USER_H__ */
