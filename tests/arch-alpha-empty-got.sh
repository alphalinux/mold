#!/usr/bin/env bash
. $(dirname $0)/common.inc

# GP still has to have a value when .alpha_got is empty.
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
status=0
$QEMU $t/exe || status=$?
[ $status = 42 ]
