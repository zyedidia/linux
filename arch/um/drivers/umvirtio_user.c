// SPDX-License-Identifier: GPL-2.0
/*
 * umvirtio: host-side helpers.
 *
 * Ordinary host userspace. Built with USER_CFLAGS (see
 * arch/um/scripts/Makefile.rules, which picks up *_user.o automatically),
 * so libc is available and kernel headers are not.
 */

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "umvirtio_user.h"

int umv_user_connect(const char *path)
{
	struct sockaddr_un addr;
	int fd, rc;

	if (!path || strlen(path) >= sizeof(addr.sun_path))
		return -EINVAL;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -errno;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strcpy(addr.sun_path, path);

	rc = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
	if (rc < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}

	return fd;
}

int umv_user_eventfd(void)
{
	int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	return fd < 0 ? -errno : fd;
}

int umv_user_eventfd_ack(int fd)
{
	unsigned long long counter;
	int rc;

	do {
		rc = read(fd, &counter, sizeof(counter));
	} while (rc < 0 && errno == EINTR);

	if (rc < 0 && errno != EAGAIN)
		return -errno;

	return 0;
}

int umv_user_eventfd_signal(int fd)
{
	unsigned long long one = 1;
	int rc;

	do {
		rc = write(fd, &one, sizeof(one));
	} while (rc < 0 && errno == EINTR);

	return rc < 0 ? -errno : 0;
}

int umv_user_send_fds(int sock, const void *buf, int len,
		      const int *fds, int nfds)
{
	/* Sized for the largest batch we ever send: 1 + 2 * UMV_MAX_QUEUES. */
	char control[CMSG_SPACE(sizeof(int) * (1 + 2 * UMV_MAX_QUEUES))];
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	int rc;

	if (nfds < 0 || nfds > 1 + 2 * UMV_MAX_QUEUES)
		return -EINVAL;

	memset(control, 0, sizeof(control));
	memset(&msg, 0, sizeof(msg));

	iov.iov_base = (void *)buf;
	iov.iov_len = len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
	memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * nfds);

	do {
		rc = sendmsg(sock, &msg, MSG_NOSIGNAL);
	} while (rc < 0 && errno == EINTR);

	if (rc < 0)
		return -errno;

	/*
	 * A short send would split the fd batch from its header with no way
	 * to resynchronise, so treat it as fatal rather than retrying.
	 */
	return rc == len ? 0 : -EIO;
}

int umv_user_send(int sock, const void *buf, int len)
{
	const char *p = buf;
	int left = len;

	while (left > 0) {
		int rc = send(sock, p, left, MSG_NOSIGNAL);

		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (rc == 0)
			return -EPIPE;

		p += rc;
		left -= rc;
	}

	return len;
}

int umv_user_recv(int sock, void *buf, int len)
{
	char *p = buf;
	int left = len;

	while (left > 0) {
		int rc = recv(sock, p, left, 0);

		if (rc < 0) {
			if (errno == EINTR)
				continue;
			/*
			 * Only report EAGAIN when nothing has been consumed;
			 * once we are mid-message there is no way to hand the
			 * partial read back to the caller.
			 */
			if (errno == EAGAIN && left == len)
				return -EAGAIN;
			if (errno == EAGAIN)
				continue;
			return -errno;
		}
		if (rc == 0)
			return left == len ? 0 : -EPIPE;

		p += rc;
		left -= rc;
	}

	return len;
}

void umv_user_close(int fd)
{
	if (fd >= 0)
		close(fd);
}
