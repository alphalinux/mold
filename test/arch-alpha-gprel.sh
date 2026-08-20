#!/usr/bin/env bash
. $(dirname $0)/common.inc

# A GP-relative reference is resolved at link-time as the distance
# between the symbol and GP, so it works only for a symbol that is
# defined in the output file. This program has no GOT entry at all, so it
# also covers the GP value we pick when .alpha_got is empty.
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.text
	.globl	_start
	.ent	_start
_start:
	br	\$27, 1f
1:	ldgp	\$29, 0(\$27)
	ldah	\$1, myvar(\$29)	!gprelhigh
	ldq	\$16, myvar(\$1)	!gprellow
	lda	\$0, 1		# __NR_exit
	callsys
	.end	_start

	.data
	.globl	myvar
myvar:	.quad	42
EOF

./mold -o $t/exe $t/a.o -e _start
! readelf -WS $t/exe | grep -q '\.alpha_got'
$QEMU $t/exe || [ $? = 42 ]

# GP-relative relocations against an imported symbol can't be resolved.
cat <<EOF | $CC -c -o $t/b.o -fPIC -xassembler -
	.text
	.globl	get_var
	.ent	get_var
get_var:
	ldgp	\$29, 0(\$27)
	.prologue 1
	ldah	\$1, imported(\$29)	!gprelhigh
	ldq	\$0, imported(\$1)	!gprellow
	ret
	.end	get_var
EOF

cat <<EOF | $CC -c -o $t/c.o -fPIC -xc -
int imported = 5;
EOF

$CC -B. -shared -o $t/d.so $t/c.o

! $CC -B. -shared -o $t/e.so $t/b.o $t/d.so 2> $t/log
grep -q 'R_ALPHA_GPRELHIGH relocation .* can not be used' $t/log
