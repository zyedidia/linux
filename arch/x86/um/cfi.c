// SPDX-License-Identifier: GPL-2.0
/*
 * This file implements Clang Control Flow Integrity (kCFI) support for
 * UML/x86-64. It is based on arch/x86/kernel/cfi.c.
 */

#include <linux/cfi.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <asm/ptrace.h>

/* The registers are listed in the order of their x86 numbers. */
static const int cfi_regs[16] = {
	HOST_AX, HOST_CX, HOST_DX, HOST_BX, HOST_SP, HOST_BP, HOST_SI, HOST_DI,
	HOST_R8, HOST_R9, HOST_R10, HOST_R11, HOST_R12, HOST_R13, HOST_R14,
	HOST_R15,
};

/*
 * The compiler checks each indirect call with this sequence:
 *
 *   movl $-<type>, %<hash>        41 b8+<hash> <type>
 *   addl -<pos>(%<reg>), %<hash>  REX 03 ModRM [SIB] <pos>
 *   je   1f                       74 02
 *   ud2                           0f 0b, at the trap address
 *   1:
 *
 * The hash register is one of r8-r15, usually r10d or r11d. The addl has a
 * SIB byte when the target is in r12. The movl gives the expected type, and
 * the addl gives the register that holds the call target.
 */
static bool decode_cfi_insn(struct pt_regs *regs, unsigned long *target,
			    u32 *type)
{
	unsigned long ip = UPT_IP(&regs->regs);
	u8 insn[13], *mov, *add, modrm;
	int reg;

	if (copy_from_kernel_nofault(insn, (void *)ip - sizeof(insn),
				     sizeof(insn)))
		return false;

	if (insn[11] != 0x74 || insn[12] != 0x02)
		return false;

	/* The addl ends at the je, with or without a SIB byte. */
	if (insn[8] == 0x03 && (insn[9] & 0x07) != 4)
		add = &insn[7];
	else if (insn[7] == 0x03 && (insn[8] & 0x07) == 4 && insn[9] == 0x24)
		add = &insn[6];
	else
		return false;
	mov = add - 6;
	modrm = add[2];

	/*
	 * The REX prefix must have R set, which selects r8-r15 for the hash,
	 * and W and X clear. The ModRM mod field must be 1.
	 */
	if ((add[0] & 0xfe) != 0x44 || (modrm & 0xc0) != 0x40)
		return false;
	if (mov[0] != 0x41 || mov[1] != (0xb8 | ((modrm >> 3) & 0x07)))
		return false;

	*type = -get_unaligned((u32 *)&mov[2]);
	reg = ((add[0] & 0x01) << 3) | (modrm & 0x07);
	*target = regs->regs.gp[cfi_regs[reg]];
	return true;
}

/*
 * Checks whether a ud2 trap is a CFI failure, and reports it if it is. The
 * return value is a bug_trap_type, like that of report_bug().
 */
enum bug_trap_type handle_cfi_failure(struct pt_regs *regs)
{
	unsigned long addr = UPT_IP(&regs->regs);
	unsigned long target;
	u32 type;

	if (!is_cfi_trap(addr))
		return BUG_TRAP_TYPE_NONE;
	if (!decode_cfi_insn(regs, &target, &type))
		return report_cfi_failure_noaddr(regs, addr);
	return report_cfi_failure(regs, addr, &target, type);
}

/*
 * Ensure that __kcfi_typeid_ symbols are emitted for functions that may
 * not be indirectly called with all configurations.
 */
__ADDRESSABLE(__memcpy)
