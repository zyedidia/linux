/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_IO_H
#define _ASM_UM_IO_H
#include <linux/types.h>

#ifdef CONFIG_INDIRECT_IOMEM_FALLBACK
/*
 * Fallback accessors for __iomem pointers that were not handed out by
 * logic_iomem's emulated ioremap(), i.e. direct mappings of host memory
 * such as mmap()ed VFIO BARs. These are plain accesses in the UML
 * process' address space; any mapping attributes (e.g. uncached device
 * memory) come from the host mapping itself.
 */
static inline u8 real_raw_readb(const volatile void __iomem *addr)
{
	return *(const volatile u8 __force *)addr;
}
#define real_raw_readb real_raw_readb

static inline u16 real_raw_readw(const volatile void __iomem *addr)
{
	return *(const volatile u16 __force *)addr;
}
#define real_raw_readw real_raw_readw

static inline u32 real_raw_readl(const volatile void __iomem *addr)
{
	return *(const volatile u32 __force *)addr;
}
#define real_raw_readl real_raw_readl

#ifdef CONFIG_64BIT
static inline u64 real_raw_readq(const volatile void __iomem *addr)
{
	return *(const volatile u64 __force *)addr;
}
#define real_raw_readq real_raw_readq
#endif /* CONFIG_64BIT */

static inline void real_raw_writeb(u8 value, volatile void __iomem *addr)
{
	*(volatile u8 __force *)addr = value;
}
#define real_raw_writeb real_raw_writeb

static inline void real_raw_writew(u16 value, volatile void __iomem *addr)
{
	*(volatile u16 __force *)addr = value;
}
#define real_raw_writew real_raw_writew

static inline void real_raw_writel(u32 value, volatile void __iomem *addr)
{
	*(volatile u32 __force *)addr = value;
}
#define real_raw_writel real_raw_writel

#ifdef CONFIG_64BIT
static inline void real_raw_writeq(u64 value, volatile void __iomem *addr)
{
	*(volatile u64 __force *)addr = value;
}
#define real_raw_writeq real_raw_writeq
#endif /* CONFIG_64BIT */

void __iomem *real_ioremap(phys_addr_t offset, size_t size);
#define real_ioremap real_ioremap

void real_iounmap(volatile void __iomem *addr);
#define real_iounmap real_iounmap

void real_memset_io(volatile void __iomem *addr, int value, size_t size);
#define real_memset_io real_memset_io

void real_memcpy_fromio(void *buffer, const volatile void __iomem *addr,
			size_t size);
#define real_memcpy_fromio real_memcpy_fromio

void real_memcpy_toio(volatile void __iomem *addr, const void *buffer,
		      size_t size);
#define real_memcpy_toio real_memcpy_toio
#endif /* CONFIG_INDIRECT_IOMEM_FALLBACK */

/* get emulated iomem (if desired) */
#include <asm-generic/logic_io.h>

#ifndef ioremap
#define ioremap ioremap
static inline void __iomem *ioremap(phys_addr_t offset, size_t size)
{
	return NULL;
}
#endif /* ioremap */

#ifndef iounmap
#define iounmap iounmap
static inline void iounmap(void __iomem *addr)
{
}
#endif /* iounmap */

#include <asm-generic/io.h>

#endif
