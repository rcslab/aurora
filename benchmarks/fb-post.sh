#!/usr/local/bin/bash
touch tmp
grep -E "ms]" $1 | awk '{ printf "%s, %s, %s, %s, %s\n", $1, $2, $3, $4, $5 }' >> tmp
mv tmp $1
