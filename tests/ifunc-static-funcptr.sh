#!/usr/bin/env bash
. $(dirname $0)/common.inc

test_cflags -static || skip
supports_ifunc || skip

cat <<EOF | $CC -o $t/a.o -c -xc -
#include <stdio.h>

static int real_foo(void) { return 3; }
static void *resolve_foo(void) { return real_foo; }
int foo(void) __attribute__((ifunc("resolve_foo")));

int (*fp)(void) = foo;

int main() {
  printf("%d %d\n", foo(), fp());
  return 0;
}
EOF

$CC -B. -o $t/exe $t/a.o -static
$QEMU $t/exe | grep '^3 3$'

# __rel[a]_iplt_start and __rel[a]_iplt_end must bracket all IRELATIVE
# relocations, including one for `fp` if we emitted it.
start=$(readelf -Ws $t/exe | awk '$8 ~ /^__rela?_iplt_start$/ { print $2 }')
end=$(readelf -Ws $t/exe | awk '$8 ~ /^__rela?_iplt_end$/ { print $2 }')
entsize=$(readelf -WS $t/exe | grep -E ' \.rela?\.dyn ' | sed 's/.*\] //' | awk '{ print $6 }')
num=$(readelf -Wr $t/exe | grep -c IRELATIVE || true)
[ $(( 0x$end - 0x$start )) -eq $(( num * 0x$entsize )) ]
