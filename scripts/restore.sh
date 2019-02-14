#!/usr/local/bin/bash

FD="1"
MEM="2"
SNAPST="$HOME/snapshot"
DIR="/usr/home/etsal/slsmm/tools/"

if [ "$#" -eq 0 ]; then
	echo "Usage: ./restore.sh <mem|fd> [indices of checkpoints]"
	exit
fi

# Whether we restore from memory or files
TYPE="$FD"
if [ "$1" = "mem" ]; then
	TYPE="$MEM"
fi

FILES="$SNAPST"
if [ "$#" -ge 2 ] && [ "$TYPE" = "$FD" ]; then
	FILES=""
	for arg in "${@:2}"; do
		if [ "$TYPE" = "$FD" ]; then
			FILES="$FILES snapshot$arg"
		else 
			FILES="$FILES $arg"
		fi
	done
fi

"$DIR"/procrestore/procrestore "$TYPE" "$FILES"

