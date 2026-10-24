// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 */

#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/initrd.h>
#include <linux/kstrtox.h>
#include <linux/string.h>
#include <asm/types.h>
#include <init.h>
#include <os.h>

#include "um_arch.h"

/* Changed by uml_initrd_setup, which is a setup */
static char *initrd __initdata = NULL;

int __init read_initrd(void)
{
	unsigned long long size;
	void *area;
	int fd;

	if (!initrd)
		return 0;

	if (strstarts(initrd, "fd:")) {
		if (kstrtoint(initrd + 3, 10, &fd) || fd <= 2) {
			printk(KERN_ERR "Invalid initrd fd '%s'\n", initrd);
			return 0;
		}
		area = uml_load_fd(fd, initrd, &size);
		os_close_file(fd);
	} else {
		area = uml_load_file(initrd, &size);
	}
	if (!area)
		return 0;

	initrd_start = (unsigned long) area;
	initrd_end = initrd_start + size;
	return 0;
}

static int __init uml_initrd_setup(char *line, int *add)
{
	*add = 0;
	initrd = line;
	return 0;
}

__uml_setup("initrd=", uml_initrd_setup,
"initrd=<initrd image>\n"
"initrd=fd:<n>\n"
"    This is used to boot UML from an initrd image.  The argument is the\n"
"    name of the file containing the image, or fd:<n> for an image in an\n"
"    open host file descriptor, which is closed after it is read.\n\n"
);
