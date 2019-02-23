#!/usr/local/bin/bash

SNAPST="$HOME/snapshot"
DIR="/usr/home/etsal/slsmm/tools/"
MEM="memory"
PROG="/stressdump/stressdump"

if [ "$#" -eq 1 ]; then 
	"$DIR"/"$PROG" "$SNAPST" `pidof $1` 0
# FIXME: The way this works we can't do incremental checkpoints in memory mode
elif  [ "$#" -eq 2 ] && [ "$2" -eq 0 ]; then
	"$DIR"/"$PROG" "$MEM" `pidof $1` 0

elif [ "$#" -eq 2 ]; then
	"$DIR"/"$PROG" "$SNAPST$2" `pidof $1` 1

else 
	echo "Usage: ./checkpoint.sh <test name> [index appended to snapshot]"
fi

