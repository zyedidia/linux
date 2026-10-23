/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_UM_IO_H
#define _ASM_UM_IO_H
#include <linux/types.h>

#ifdef CONFIG_INDIRECT_IOMEM_FALLBACK
#define UM_REAL_IO_OPS(op, sz)						\
static inline u##sz real_raw_read##op(const volatile void __iomem *addr)\
{									\
	return *(const volatile u##sz __force *)addr;			\
}									\
									\
static inline void real_raw_write##op(u##sz val,			\
				      volatile void __iomem *addr)	\
{									\
	*(volatile u##sz __force *)addr = val;				\
}

UM_REAL_IO_OPS(b, 8)
UM_REAL_IO_OPS(w, 16)
UM_REAL_IO_OPS(l, 32)
#define real_raw_readb real_raw_readb
#define real_raw_writeb real_raw_writeb
#define real_raw_readw real_raw_readw
#define real_raw_writew real_raw_writew
#define real_raw_readl real_raw_readl
#define real_raw_writel real_raw_writel
#ifdef CONFIG_64BIT
UM_REAL_IO_OPS(q, 64)
#define real_raw_readq real_raw_readq
#define real_raw_writeq real_raw_writeq
#endif

#define real_ioremap real_ioremap
void __iomem *real_ioremap(phys_addr_t offset, size_t size);
#define real_iounmap real_iounmap
void real_iounmap(volatile void __iomem *addr);
#define real_memset_io real_memset_io
void real_memset_io(volatile void __iomem *addr, int value, size_t size);
#define real_memcpy_fromio real_memcpy_fromio
void real_memcpy_fromio(void *buffer, const volatile void __iomem *addr,
			size_t size);
#define real_memcpy_toio real_memcpy_toio
void real_memcpy_toio(volatile void __iomem *addr, const void *buffer,
		      size_t size);
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
