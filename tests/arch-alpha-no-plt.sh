#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Alpha has neither a PLT nor copy relocations, so an imported symbol
# has no link-time address, and R_ALPHA_SREL32 against one has to be
# rejected.
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
int imported_data = 42;
EOF

$CC -B. -shared -o $t/c.so $t/b.o

! $CC -B. -o $t/exe $t/a.o $t/c.so 2> $t/log
grep -q 'R_ALPHA_SREL32 against an imported symbol' $t/log

cat <<EOF | $CC -c -o $t/a2.o -xassembler -
	.text
	.globl	get_offset
	.ent	get_offset
get_offset:
	ldgp	\$29, 0(\$27)
	.prologue 1
	.long	imported_data - .
	ret
	.end	get_offset
EOF

! $CC -B. -no-pie -o $t/exe $t/a2.o $t/c.so 2> $t/log
grep -q 'R_ALPHA_SREL32 against an imported symbol' $t/log

# Taking the address of an imported symbol from a read-only section of a
# position-dependent executable leaves a dynamic relocation behind rather
# than creating a canonical PLT or a copy relocation.
cat <<EOF | $CC -c -o $t/d.o -xc -
void imported(void);
extern int imported_data;
void (*const fp)(void) = imported;
int *const dp = &imported_data;
int main(void) { fp(); return *dp != 42; }
EOF

$CC -B. -no-pie -o $t/exe2 $t/d.o $t/c.so
readelf -Wr $t/exe2 > $t/log
grep -Eq 'R_ALPHA_REFQUAD .* imported \+ 0' $t/log
grep -Eq 'R_ALPHA_REFQUAD .* imported_data \+ 0' $t/log
! grep -Fq R_ALPHA_COPY $t/log
$QEMU $t/exe2
