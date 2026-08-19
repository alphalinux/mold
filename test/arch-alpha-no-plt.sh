#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Alpha calls every function indirectly through GOT, so mold creates no
# PLT for it. That means there's no link-time address we can give to an
# imported function, and a PC-relative relocation can't be promoted to a
# dynamic relocation, so R_ALPHA_SREL32 against one has to be rejected.
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.text
	.globl	get_offset
	.ent	get_offset
get_offset:
	ldgp	\$29, 0(\$27)
	.prologue 1
	.long	imported - .
	ret
	.end	get_offset
EOF

cat <<EOF | $CC -c -o $t/b.o -fPIC -xc -
void imported(void) {}
EOF

$CC -B. -shared -o $t/c.so $t/b.o

! $CC -B. -o $t/exe $t/a.o $t/c.so 2> $t/log
grep -q 'R_ALPHA_SREL32 against an imported function' $t/log

# Taking the address of an imported function from a read-only section of
# a position-dependent executable leaves a dynamic relocation behind
# rather than promoting the symbol to a canonical PLT that doesn't exist.
cat <<EOF | $CC -c -o $t/d.o -xc -
void imported(void);
void (*const fp)(void) = imported;
int main(void) { fp(); return 0; }
EOF

$CC -B. -no-pie -o $t/exe2 $t/d.o $t/c.so
readelf -Wr $t/exe2 | grep -Fq 'R_ALPHA_REFQUAD'
$QEMU $t/exe2
