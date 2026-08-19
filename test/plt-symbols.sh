#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Alpha does not use PLT because all function calls are made indirectly
# through GOT.
[ $MACHINE = alpha ] && skip

cat <<EOF | $CC -c -o $t/a.o -xc - -fPIC
void bar();
void foo() { bar(); }
EOF

$CC -B. -shared -o $t/b.so $t/a.o
readelf -Ws $t/b.so | grep 'LOCAL.*bar\$plt$'
