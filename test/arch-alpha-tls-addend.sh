#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Unlike most targets, Alpha's TLS GOT relocations can carry a non-zero
# addend, so a GOT entry is keyed by a symbol and an addend just as
# R_ALPHA_LITERAL's is. Both of these read tlsvar[2].
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.text
	.globl	get_ie
	.ent	get_ie
get_ie:
	ldgp	\$29, 0(\$27)
	.prologue 1
	ldq	\$1, tlsvar+8(\$29)	!gottprel
	call_pal 0x9e			# rduniq
	addq	\$1, \$0, \$1
	ldl	\$0, 0(\$1)
	ret
	.end	get_ie
EOF

cat <<EOF | $CC -c -o $t/b.o -fPIC -xassembler -
	.text
	.globl	get_gd
	.ent	get_gd
get_gd:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$30, -16(\$30)
	stq	\$26, 0(\$30)
	lda	\$16, tlsvar+8(\$29)		!tlsgd
	ldq	\$27, __tls_get_addr(\$29)	!literal
	jsr	\$26, (\$27), __tls_get_addr
	ldgp	\$29, 0(\$26)
	ldl	\$0, 0(\$0)
	ldq	\$26, 0(\$30)
	lda	\$30, 16(\$30)
	ret
	.end	get_gd
EOF

cat <<EOF | $CC -c -o $t/c.o -xc -
#include <stdio.h>
__thread int tlsvar[4] = {11, 22, 33, 44};
int get_ie(void);
int get_gd(void);
int main(void) { printf("%d %d\n", get_ie(), get_gd()); }
EOF

$CC -B. -shared -o $t/d.so $t/b.o
$CC -B. -o $t/exe $t/a.o $t/c.o $t/d.so
$QEMU $t/exe | grep -q '^33 33$'
