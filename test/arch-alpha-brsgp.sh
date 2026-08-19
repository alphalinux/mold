#!/usr/bin/env bash
. $(dirname $0)/common.inc

# R_ALPHA_BRSGP is a direct call to a function that is known to use the
# same GP as the caller. The branch has to skip the callee's `ldgp`
# prologue, because a direct branch doesn't set $27 to the callee's
# address.
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.text
	.globl	callee
	.ent	callee
callee:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$0, 42(\$31)
	ret
	.end	callee

	.globl	docall
	.ent	docall
docall:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$30, -16(\$30)
	stq	\$26, 0(\$30)
	bsr	\$26, callee	!samegp
	ldq	\$26, 0(\$30)
	lda	\$30, 16(\$30)
	ret
	.end	docall
EOF

cat <<EOF | $CC -c -o $t/b.o -xc -
#include <stdio.h>
int docall(void);
int main(void) { printf("%d\n", docall()); }
EOF

$CC -B. -o $t/exe $t/a.o $t/b.o
$OBJDUMP -d --disassemble=docall $t/exe | grep -E 'bsr\s+ra,.*<callee\+0x8>'
$QEMU $t/exe | grep -q '^42$'

# We have no PLT to indirect through, so a BRSGP call to an imported
# symbol can't be resolved.
cat <<EOF | $CC -c -o $t/c.o -xassembler -
	.text
	.globl	callimported
	.ent	callimported
callimported:
	ldgp	\$29, 0(\$27)
	.prologue 1
	bsr	\$26, imported	!samegp
	ret
	.end	callimported
EOF

cat <<EOF | $CC -c -o $t/d.o -fPIC -xc -
void imported(void) {}
EOF

$CC -B. -shared -o $t/e.so $t/d.o
! $CC -B. -o $t/exe2 $t/c.o $t/b.o $t/e.so 2> $t/log
grep -q 'against an imported symbol is not supported' $t/log
