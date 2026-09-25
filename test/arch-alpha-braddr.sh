#!/usr/bin/env bash
. $(dirname $0)/common.inc

# R_ALPHA_BRADDR is a plain 21-bit branch displacement. glibc's libc.a
# uses one to branch from its division routines to __divbyzero.
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.globl	_start
	.ent	_start
_start:
	.prologue 0
	beq	\$1, target
	ret
	.end	_start
EOF

cat <<EOF | $CC -c -o $t/b.o -xassembler -
	.text
	.space	0x1000
	.globl	target
	.ent	target
target:
	.prologue 0
	ret
	.end	target
EOF

./mold -static -o $t/exe $t/a.o $t/b.o
$OBJDUMP -d $t/exe | grep -Eq 'beq.*<target>'
