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

# GPREL32 and a GPRELHIGH/GPRELLOW pair hold a 32-bit displacement.
cat <<EOF | $CC -c -o $t/f.o -xassembler -
	.text
	.globl	get_far
	.ent	get_far
get_far:
	ldgp	\$29, 0(\$27)
	.prologue 1
	ldah	\$1, far(\$29)	!gprelhigh
	ldq	\$0, far(\$1)	!gprellow
	ret
	.end	get_far
	.section .rodata
	.gprel32 far
EOF

! $CC -B. -no-pie -o $t/exe $t/f.o $t/c.o -Wl,--defsym=far=0x100000000000 2> $t/log
grep -q 'R_ALPHA_GPRELHIGH .* out of range' $t/log
grep -q 'R_ALPHA_GPREL32 .* out of range' $t/log
