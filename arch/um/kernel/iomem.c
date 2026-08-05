// SPDX-License-Identifier: GPL-2.0
/*
 * Out-of-line real_* fallbacks for logic_iomem
 * (CONFIG_INDIRECT_IOMEM_FALLBACK).
 *
 * These handle __iomem pointers that logic_iomem did not hand out
 * itself, i.e. direct mappings of host memory such as mmap()ed VFIO
 * BARs. The bulk operations only guarantee naturally aligned accesses
 * on the IO side, since the host mapping may be uncached device memory
 * that faults on unaligned access.
 */
#include <linux/types.h>
#include <linux/align.h>
#include <linux/bug.h>
#include <linux/unaligned.h>
#include <asm/io.h>

void __iomem *real_ioremap(phys_addr_t offset, size_t size)
{
	WARN(1, "invalid ioremap(0x%llx, 0x%zx)\n",
	     (unsigned long long)offset, size);
	return NULL;
}

void real_iounmap(volatile void __iomem *addr)
{
	/*
	 * Direct mappings are owned by whoever created them (e.g. the
	 * VFIO driver mmap()ing a BAR), so there is nothing to do here.
	 */
}

void real_memset_io(volatile void __iomem *addr, int value, size_t size)
{
	u64 pattern = (u8)value * 0x0101010101010101ULL;

	while (size) {
		unsigned long a = (unsigned long)addr;

#ifdef CONFIG_64BIT
		if (size >= 8 && IS_ALIGNED(a, 8)) {
			real_raw_writeq(pattern, addr);
			addr += 8;
			size -= 8;
		} else
#endif
		if (size >= 4 && IS_ALIGNED(a, 4)) {
			real_raw_writel((u32)pattern, addr);
			addr += 4;
			size -= 4;
		} else if (size >= 2 && IS_ALIGNED(a, 2)) {
			real_raw_writew((u16)pattern, addr);
			addr += 2;
			size -= 2;
		} else {
			real_raw_writeb((u8)value, addr);
			addr += 1;
			size -= 1;
		}
	}
}

void real_memcpy_fromio(void *buffer, const volatile void __iomem *addr,
			size_t size)
{
	u8 *buf = buffer;

	while (size) {
		unsigned long a = (unsigned long)addr;

#ifdef CONFIG_64BIT
		if (size >= 8 && IS_ALIGNED(a, 8)) {
			put_unaligned(real_raw_readq(addr), (u64 *)buf);
			buf += 8;
			addr += 8;
			size -= 8;
		} else
#endif
		if (size >= 4 && IS_ALIGNED(a, 4)) {
			put_unaligned(real_raw_readl(addr), (u32 *)buf);
			buf += 4;
			addr += 4;
			size -= 4;
		} else if (size >= 2 && IS_ALIGNED(a, 2)) {
			put_unaligned(real_raw_readw(addr), (u16 *)buf);
			buf += 2;
			addr += 2;
			size -= 2;
		} else {
			*buf = real_raw_readb(addr);
			buf += 1;
			addr += 1;
			size -= 1;
		}
	}
}

void real_memcpy_toio(volatile void __iomem *addr, const void *buffer,
		      size_t size)
{
	const u8 *buf = buffer;

	while (size) {
		unsigned long a = (unsigned long)addr;

#ifdef CONFIG_64BIT
		if (size >= 8 && IS_ALIGNED(a, 8)) {
			real_raw_writeq(get_unaligned((const u64 *)buf), addr);
			buf += 8;
			addr += 8;
			size -= 8;
		} else
#endif
		if (size >= 4 && IS_ALIGNED(a, 4)) {
			real_raw_writel(get_unaligned((const u32 *)buf), addr);
			buf += 4;
			addr += 4;
			size -= 4;
		} else if (size >= 2 && IS_ALIGNED(a, 2)) {
			real_raw_writew(get_unaligned((const u16 *)buf), addr);
			buf += 2;
			addr += 2;
			size -= 2;
		} else {
			real_raw_writeb(*buf, addr);
			buf += 1;
			addr += 1;
			size -= 1;
		}
	}
}
