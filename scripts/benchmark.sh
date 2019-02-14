#!/usr/local/bin/bash

if [ "$#" -ne 1 ]; then
	echo "Usage: ./test.sh <test name>"
	exit
fi

FILE=''
if [ "$1" = 'fd' ] || [ "$1" = 'mmap' ]; then
	FILE="$HOME/f"
fi

/usr/home/etsal/slsmm/tests/"$1"/"$1" $FILE

