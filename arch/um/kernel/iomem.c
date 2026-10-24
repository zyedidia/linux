// SPDX-License-Identifier: GPL-2.0
/*
 * Accessors for IO memory that is mapped directly into the UML process.
 */
#include <linux/align.h>
#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/extable.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <kern_util.h>

static atomic64_t uml_iomem_fault_count;

/*
 * Return the address to continue at after a bus error at ip, or 0 if ip is
 * not an access by one of the accessors in asm/io.h. This runs in the SIGBUS
 * handler.
 */
unsigned long uml_iomem_fixup(unsigned long ip)
{
	const struct exception_table_entry *e;

	e = search_exception_tables(ip);
	if (!e || FIELD_GET(EX_DATA_TYPE_MASK, e->data) != EX_TYPE_DEFAULT)
		return 0;

	atomic64_inc(&uml_iomem_fault_count);
	return (unsigned long)&e->fixup + e->fixup;
}

/* Return the number of accesses that faulted and were skipped. */
u64 uml_iomem_faults(void)
{
	return atomic64_read(&uml_iomem_fault_count);
}

void __iomem *real_ioremap(phys_addr_t offset, size_t size)
{
	WARN(1, "invalid ioremap(0x%llx, 0x%zx)\n",
	     (unsigned long long)offset, size);
	return NULL;
}

void real_iounmap(volatile void __iomem *addr)
{
}

/*
 * Return the size of the largest naturally aligned access at addr that does
 * not exceed size.
 */
static size_t um_io_width(const volatile void __iomem *addr, size_t size)
{
	unsigned long a = (unsigned long)addr;

	if (IS_ENABLED(CONFIG_64BIT) && size >= 8 && IS_ALIGNED(a, 8))
		return 8;
	if (size >= 4 && IS_ALIGNED(a, 4))
		return 4;
	if (size >= 2 && IS_ALIGNED(a, 2))
		return 2;
	return 1;
}

void real_memset_io(volatile void __iomem *addr, int value, size_t size)
{
	u64 val = 0x0101010101010101ULL * (u8)value;
	size_t n;

	for (; size; size -= n, addr += n) {
		n = um_io_width(addr, size);
		switch (n) {
#ifdef CONFIG_64BIT
		case 8:
			real_raw_writeq(val, addr);
			break;
#endif
		case 4:
			real_raw_writel(val, addr);
			break;
		case 2:
			real_raw_writew(val, addr);
			break;
		default:
			real_raw_writeb(val, addr);
			break;
		}
	}
}

void real_memcpy_fromio(void *buffer, const volatile void __iomem *addr,
			size_t size)
{
	u8 *buf = buffer;
	size_t n;

	for (; size; size -= n, addr += n, buf += n) {
		n = um_io_width(addr, size);
		switch (n) {
#ifdef CONFIG_64BIT
		case 8:
			put_unaligned(real_raw_readq(addr), (u64 *)buf);
			break;
#endif
		case 4:
			put_unaligned(real_raw_readl(addr), (u32 *)buf);
			break;
		case 2:
			put_unaligned(real_raw_readw(addr), (u16 *)buf);
			break;
		default:
			*buf = real_raw_readb(addr);
			break;
		}
	}
}

void real_memcpy_toio(volatile void __iomem *addr, const void *buffer,
		      size_t size)
{
	const u8 *buf = buffer;
	size_t n;

	for (; size; size -= n, addr += n, buf += n) {
		n = um_io_width(addr, size);
		switch (n) {
#ifdef CONFIG_64BIT
		case 8:
			real_raw_writeq(get_unaligned((const u64 *)buf), addr);
			break;
#endif
		case 4:
			real_raw_writel(get_unaligned((const u32 *)buf), addr);
			break;
		case 2:
			real_raw_writew(get_unaligned((const u16 *)buf), addr);
			break;
		default:
			real_raw_writeb(*buf, addr);
			break;
		}
	}
}
