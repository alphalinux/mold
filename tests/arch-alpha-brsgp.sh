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

# The callee must be marked as either skipping its GP load or not using
# GP at all.
cat <<EOF | $CC -c -o $t/f.o -xassembler -
	.text
	.globl	noprologue
	.hidden	noprologue
noprologue:
	ret
	.globl	callnoprologue
	.ent	callnoprologue
callnoprologue:
	ldgp	\$29, 0(\$27)
	.prologue 1
	bsr	\$26, noprologue	!samegp
	ret
	.end	callnoprologue
EOF

! $CC -B. -shared -o $t/g.so $t/f.o 2> $t/log
grep -q 'against a symbol without .prologue' $t/log

# The caller and the callee must be assigned the same GP. Each of these
# two files refers to 5000 GOT entries, so they can't share a GOT group.
gen() {
  echo '.data'
  seq 0 4999 | sed "s/.*/.globl $1&\n$1&: .quad 0/"
  echo '.text'
  echo ".globl $1func"
  echo ".hidden $1func"
  echo ".ent $1func"
  echo "$1func:"
  echo 'ldgp $29, 0($27)'
  echo '.prologue 1'
  seq 0 4999 | sed "s/.*/ldq \$1, $1&(\$29) !literal/"
  [ -n "$2" ] && echo "bsr \$26, $2 !samegp"
  echo 'ret'
  echo ".end $1func"
}

gen x yfunc | $CC -c -o $t/x.o -xassembler -
gen y | $CC -c -o $t/y.o -xassembler -

! $CC -B. -shared -o $t/h.so $t/x.o $t/y.o 2> $t/log
grep -q 'BRSGP call between input files that are assigned different GP values' $t/log
