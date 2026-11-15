#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
#
# Check that the code of a UML binary has WRPKRU instructions only in the
# protection key gates, and XRSTOR and XRSTORS, which can also load the PKRU,
# only in glibc's lazy binding resolvers, and fail otherwise. The same bytes
# inside other instructions, such as in a displacement, are reported as a
# warning. Reaching them needs a jump into the middle of an instruction,
# which shadow stacks and kCFI make hard, and removing them would need the
# instructions around them rewritten. It also fails if a segment of the
# binary is both writable and executable.
#
# usage: mpk-gadgets.sh <objdump> <objcopy> <nm> <binary>

set -e

objdump=$1
objcopy=$2
nm=$3
bin=$4

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Print the start and size of each symbol, in hex.
"$nm" -S "$bin" > "$tmp/syms"

sym() {
	awk -v s="$1" '$NF == s { print $1; exit }' "$tmp/syms"
}

sym_range() {
	awk -v s="$1" 'NF == 4 && $4 == s { print $1, $2; exit }' "$tmp/syms"
}

gate_start=$(sym uml_mpk_gate_start)
gate_end=$(sym uml_mpk_gate_end)
if [ -z "$gate_start" ] || [ -z "$gate_end" ]; then
	echo "$bin: no protection key gates" >&2
	exit 1
fi
gates="$gate_start $(printf '%x' $((0x$gate_end - 0x$gate_start)))"

resolvers=
for s in _dl_runtime_resolve_xsave _dl_runtime_resolve_xsavec; do
	r=$(sym_range "$s")
	[ -n "$r" ] && resolvers="$resolvers $r"
done

in_ranges() {
	x=$1
	shift
	while [ $# -ge 2 ]; do
		start=$((0x$1))
		if [ "$x" -ge "$start" ] && [ "$x" -lt $((start + 0x$2)) ]; then
			return 0
		fi
		shift 2
	done
	return 1
}

bad=0

# No segment, and not the stack, may be both writable and executable.
if "$objdump" -p "$bin" | awk '
	$1 == "LOAD" || $1 == "STACK" { load = 1; next }
	load && /flags/ { load = 0; if ($NF ~ /w/ && $NF ~ /x/) found = 1 }
	END { exit !found }'; then
	echo "$bin: a segment is writable and executable" >&2
	bad=1
fi

# The instructions, at the addresses where the disassembly decodes them.
"$objdump" -d --no-show-raw-insn "$bin" |
	awk '$2 ~ /^(wrpkru|xrstors?(64)?)$/ { sub(":", "", $1); print $1, $2 }' \
	> "$tmp/insns"

while read -r addr insn; do
	a=$((0x$addr))
	echo "$a" >> "$tmp/decoded"
	if [ "$insn" = wrpkru ]; then
		# shellcheck disable=SC2086
		in_ranges "$a" $gates && continue
	else
		# shellcheck disable=SC2086
		in_ranges "$a" $resolvers && continue
	fi
	echo "$bin: $insn at 0x$addr outside the code allowed to load the PKRU" >&2
	bad=1
done < "$tmp/insns"
touch "$tmp/decoded"

# Print the offsets in the section of the bytes that a pattern matches.
scan() {
	status=0
	LC_ALL=C grep -obUaP "$1" "$tmp/code" > "$tmp/hits" || status=$?
	if [ "$status" -gt 1 ]; then
		echo "$0: grep -P failed" >&2
		exit 1
	fi
	cut -d: -f1 "$tmp/hits"
}

"$objdump" -h "$bin" | awk '
	/^ *[0-9]+ / { name = $2; vma = $4; next }
	/CODE/ && name { print name, vma; name = "" }
' > "$tmp/sections"

# WRPKRU is 0f 01 ef, XRSTOR is 0f ae /5 and XRSTORS is 0f c7 /3, the last
# two with a memory operand.
pattern='\x0f\x01\xef|\x0f\xae[\x28-\x2f\x68-\x6f\xa8-\xaf]|\x0f\xc7[\x18-\x1f\x58-\x5f\x98-\x9f]'
while read -r name vma; do
	"$objcopy" -O binary --only-section="$name" "$bin" "$tmp/code"
	scan "$pattern" > "$tmp/offsets"
	while read -r off; do
		a=$((0x$vma + off))
		grep -qx "$a" "$tmp/decoded" && continue
		printf '%s: warning: WRPKRU or XRSTOR bytes inside an instruction at 0x%x\n' \
			"$bin" "$a" >&2
	done < "$tmp/offsets"
done < "$tmp/sections"

exit $bad
