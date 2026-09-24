#!/usr/bin/env bash
. $(dirname $0)/common.inc

# GP-relative relocations against an imported symbol can't be resolved.
cat <<EOF | $CC -c -o $t/a.o -fPIC -xassembler -
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

cat <<EOF | $CC -c -o $t/b.o -fPIC -xc -
int imported = 5;
EOF

cat <<EOF | $CC -c -o $t/c.o -xc -
int main(void) { return 0; }
EOF

$CC -B. -shared -o $t/d.so $t/b.o

! $CC -B. -shared -o $t/e.so $t/a.o $t/d.so 2> $t/log
grep -q 'R_ALPHA_GPRELHIGH against an imported symbol' $t/log

! $CC -B. -no-pie -o $t/exe $t/a.o $t/c.o $t/d.so 2> $t/log
grep -q 'R_ALPHA_GPRELHIGH against an imported symbol' $t/log
