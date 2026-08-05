/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ARCH_STUB_DATA_H
#define __ARCH_STUB_DATA_H

/*
 * There is no stub process without userspace support, so no per-arch state
 * to shuttle through it; the member keeps the shared struct layout happy.
 */
struct stub_data_arch {
	int sync;
};

#endif /* __ARCH_STUB_DATA_H */
