/* SPDX-License-Identifier: GPL-2.0 */
/*
 * umvirtio: host-side helpers.
 *
 * Everything declared here is implemented in umvirtio_user.c, which is
 * built with host libc headers and must not touch kernel state. Only
 * fixed-width types and the wire structs cross this boundary.
 */

#ifndef __UMVIRTIO_USER_H__
#define __UMVIRTIO_USER_H__

#include "umvirtio_proto.h"

/* Connect to the bridge's listening unix socket. Returns fd or -errno. */
int umv_user_connect(const char *path);

/* eventfd(2) helpers. _ack drains a kick, _signal raises a call. */
int umv_user_eventfd(void);
int umv_user_eventfd_ack(int fd);
int umv_user_eventfd_signal(int fd);

/* Send @buf with @nfds descriptors attached via SCM_RIGHTS. */
int umv_user_send_fds(int sock, const void *buf, int len,
		      const int *fds, int nfds);

int umv_user_send(int sock, const void *buf, int len);

/*
 * Read exactly @len bytes. Returns @len, 0 if the peer closed cleanly on a
 * message boundary, or -errno. EAGAIN is returned as -EAGAIN so an IRQ
 * handler can poll without blocking.
 */
int umv_user_recv(int sock, void *buf, int len);

void umv_user_close(int fd);

#endif /* __UMVIRTIO_USER_H__ */
