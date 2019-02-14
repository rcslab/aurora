#!/usr/local/bin/bash

SNAPST="$HOME/snapshot"
DIR="/usr/home/etsal/slsmm/tools/"
MEM="memory"

if [ "$#" -eq 1 ]; then 
	"$DIR"/procdump/procdump "$SNAPST" `pidof $1` 0
# FIXME: The way this works we can't do incremental checkpoints in memory mode
elif  [ "$#" -eq 2 ] && [ "$2" -eq 0 ]; then
	"$DIR"/procdump/procdump "$MEM" `pidof $1` 0

elif [ "$#" -eq 2 ]; then
	"$DIR"/procdump/procdump "$SNAPST$2" `pidof $1` 1

else 
	echo "Usage: ./checkpoint.sh <test name> [index appended to snapshot]"
fi

