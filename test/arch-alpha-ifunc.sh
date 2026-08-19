#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Older GCCs don't support the ifunc attribute on Alpha, so the generic
# ifunc tests are skipped with them. Write the ifunc symbols by hand
# instead.
#
# Alpha has no PLT; a function is called by loading its address from the
# GOT. An ifunc's GOT slot and any data word holding its address are
# therefore filled in at load-time by an R_ALPHA_IRELATIVE relocation
# whose addend is the resolver's address.
cat <<EOF | $CC -c -o $t/a.o -xassembler -
	.text
	.globl	impl
	.ent	impl
impl:
	.prologue 0
	lda	\$0, 42(\$31)
	ret
	.end	impl

	.type	local_ifunc, %gnu_indirect_function
	.ent	local_ifunc
local_ifunc:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$0, impl
	ret
	.end	local_ifunc

	.globl	global_ifunc
	.type	global_ifunc, %gnu_indirect_function
	.ent	global_ifunc
global_ifunc:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$0, impl
	ret
	.end	global_ifunc

	.globl	callit
	.ent	callit
callit:
	ldgp	\$29, 0(\$27)
	.prologue 1
	lda	\$30, -16(\$30)
	stq	\$26, 0(\$30)
	ldq	\$27, local_ifunc(\$29)	!literal!1
	jsr	\$26, (\$27), 0		!lituse_jsr!1
	ldgp	\$29, 0(\$26)
	ldq	\$26, 0(\$30)
	lda	\$30, 16(\$30)
	ret
	.end	callit

	.data
	.globl	ptr
ptr:
	.quad	local_ifunc
	.globl	gptr
gptr:
	.quad	global_ifunc
EOF

# No binutils release knows R_ALPHA_IRELATIVE (42) yet, so readelf may
# print it as an unrecognized type.
irelative() {
  readelf -Wr "$1" | grep -cE 'R_ALPHA_IRELATIVE|unrecognized: 2a'
}

# A shared object: one relocation for the GOT slot the call goes through
# and one for each of the two data words.
$CC -B. -shared -o $t/b.so $t/a.o
[ "$(irelative $t/b.so)" = 3 ]
readelf -W --dyn-syms $t/b.so |
  grep -E '(IFUNC|<OS specific>: 10).*global_ifunc'

cat <<EOF | $CC -c -o $t/c.o -xc -
#include <stdio.h>
int callit(void);
extern int (*ptr)(void);
int main(void) { printf("%d %d\n", callit(), ptr()); }
EOF

# A position-dependent executable. Other targets resolve an ifunc's
# address to its PLT entry here, but we have no PLT, so the relocations
# stay.
$CC -B. -no-pie -o $t/exe1 $t/c.o $t/a.o
[ "$(irelative $t/exe1)" = 3 ]

$CC -B. -pie -o $t/exe2 $t/c.o $t/a.o
[ "$(irelative $t/exe2)" = 3 ]

$CC -B. -o $t/exe6 $t/c.o $t/b.so

# Only a glibc that knows R_ALPHA_IRELATIVE can load these. Use GCC's
# support for the ifunc attribute as a proxy for that.
if supports_ifunc; then
  $QEMU $t/exe1 | grep -q '^42 42$'
  $QEMU $t/exe2 | grep -q '^42 42$'
  LD_LIBRARY_PATH=$t $QEMU $t/exe6 | grep -q '^42 42$'
fi

# In a statically-linked executable there is no dynamic linker, so the C
# library applies the relocations itself, between __rela_iplt_start and
# __rela_iplt_end. All of them have to be inside that range.
$CC -B. -static -nostdlib -e callit -o $t/exe3 $t/a.o
[ "$(irelative $t/exe3)" = 3 ]

readelf -Ws $t/exe3 > $t/log
start=0x$(grep -w __rela_iplt_start $t/log | awk '{print $2}')
end=0x$(grep -w __rela_iplt_end $t/log | awk '{print $2}')
[ $(( (end - start) / 24 )) = 3 ]

# An ifunc has no link-time address, so relocations that need one are
# rejected, as is an offset from the resolved address.
for insn in '.long global_ifunc - .' '.gprel32 global_ifunc' \
            'ldq $1, global_ifunc+8($29) !literal' '.quad global_ifunc+8'; do
  cat <<EOF | $CC -c -o $t/d.o -xassembler -
	.type	global_ifunc, %gnu_indirect_function
	.globl	global_ifunc
	.ent	global_ifunc
global_ifunc:
	.prologue 0
	ret
	.end	global_ifunc

	.globl	_start
	.ent	_start
_start:
	ldgp	\$29, 0(\$27)
	.prologue 1
	$insn
	ret
	.end	_start
EOF
  ! ./mold -o $t/exe4 $t/d.o 2> $t/log
  grep -q 'against an ifunc symbol' $t/log
done

# A statically-linked executable can't apply an IRELATIVE relocation to
# a read-only section.
cat <<EOF | $CC -c -o $t/e.o -xassembler -
	.section .rodata
	.quad	global_ifunc
EOF

! ./mold -static -o $t/exe5 $t/a.o $t/e.o -e callit 2> $t/log
grep -q 'ifunc symbol .* in read-only section is not supported' $t/log
