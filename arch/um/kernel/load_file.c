// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */
#include <linux/memblock.h>
#include <os.h>

#include "um_arch.h"

static void *__init uml_load(int fd, const char *name, unsigned long long size)
{
	void *area;
	int n;

	if (size == 0) {
		printk(KERN_ERR "\"%s\" is empty\n", name);
		return NULL;
	}

	area = memblock_alloc_or_panic(size, SMP_CACHE_BYTES);

	n = os_pread_file(fd, area, size, 0);
	if (n != size) {
		printk(KERN_ERR "Read of %llu bytes from '%s' failed, "
		       "err = %d\n", size, name, -n);
		memblock_free(area, size);
		return NULL;
	}

	return area;
}

void *uml_load_file(const char *filename, unsigned long long *size)
{
	void *area;
	int fd, err;

	*size = 0;

	if (!filename)
		return NULL;

	err = os_file_size(filename, size);
	if (err)
		return NULL;

	fd = os_open_file(filename, of_read(OPENFLAGS()), 0);
	if (fd < 0) {
		printk(KERN_ERR "Opening '%s' failed - err = %d\n", filename,
		       -fd);
		return NULL;
	}

	area = uml_load(fd, filename, *size);
	os_close_file(fd);
	return area;
}

void *uml_load_fd(int fd, const char *name, unsigned long long *size)
{
	struct uml_stat buf;
	int err;

	*size = 0;

	err = os_stat_fd(fd, &buf);
	if (err) {
		printk(KERN_ERR "Couldn't stat '%s' - err = %d\n", name, -err);
		return NULL;
	}
	*size = buf.ust_size;

	return uml_load(fd, name, *size);
}
