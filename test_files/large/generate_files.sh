#!/bin/bash

for (( i = 1; $i <= 128; i = $i + 1))
do
  dd if=/dev/zero of=size_$i.txt bs=1k count=$i
done

# ls -lh | cut -d ' ' -f 11 > test10.in
# sed -i -e 's/^/0 0 /' test10.in
