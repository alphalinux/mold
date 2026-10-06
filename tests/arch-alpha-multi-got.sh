#!/usr/bin/env bash
. $(dirname $0)/common.inc

# Each of 12 files refers to 3000 GOT entries, 1500 of which are shared
# with the next file. That's more than a single GP can address, so the
# GOT has to be split into groups.
for i in $(seq 0 11); do
  j=$(( (i + 1) % 12 ))
  seq 0 1499 | awk -v i=$i '{ print "long v" i "_" $1 " = " $1 ";" }' |
    $CC -c -o $t/d$i.o -fPIC -xc -
  seq 0 1499 | awk -v i=$i -v j=$j '
    { print "extern long v" i "_" $1 ", v" j "_" $1 ";" }
    END {
      print "long sum" i "(void) { long t = 0;"
      for (k = 0; k < 1500; k++)
        print "t += v" i "_" k " + v" j "_" k ";"
      print "return t; }"
    }' | $CC -c -o $t/f$i.o -fPIC -xc -
done

cat <<EOF > $t/main.c
#include <stdio.h>
EOF

for i in $(seq 0 11); do
  echo "long sum$i(void);" >> $t/main.c
done

echo 'int main(void) { long t = 0;' >> $t/main.c
for i in $(seq 0 11); do
  echo "t += sum$i();" >> $t/main.c
done
echo 'printf("%ld\n", t); }' >> $t/main.c

$CC -c -o $t/main.o $t/main.c

# 12 * 2 * (0 + 1 + ... + 1499)
expected=26982000

$CC -B. -o $t/exe1 $t/main.o $t/f*.o $t/d*.o
$QEMU $t/exe1 | grep -Fx $expected

$CC -B. -shared -o $t/libd.so $t/d*.o
$CC -B. -o $t/exe2 $t/main.o $t/f*.o $t/libd.so
$QEMU $t/exe2 | grep -Fx $expected
