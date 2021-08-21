#!/bin/sh

SLSDIR="/root/sls"
BIN="/$SLSDIR/tests/vmobject/vmobject"

source "$SLSDIR/scripts/bench.sh"

vmobject () {
	aurstripe
	aurload
	OBJCNT=$1
	SIZEKB=$(( $2 / 1024))
	SIZEMB=$(( $SIZEKB / 1024))
	RUNNO=$3
	"$BIN" "$OBJCNT" "$SIZE" > "vmobject-$OBJCNT-$SIZE-$RUNNO"
	aurunload

}

# Get numbers from 4KB to 1GB, rising exponentially. Use only one object.
for SIZEPWR in $(seq 12 30);
do
	for RUNNO in $(seq 1 2);
	do
		OBJCNT=1
		SIZE=$(dc -e "2 $SIZEPWR ^ p")
		vmobject "$OBJCNT" "$SIZE" "$RUNNO"
	done
done

# Get numbers from 1GB to 8GB, rising 1GB at a time. Use only one object.
for SIZEGB in $(seq 1 8);
do
    for RUNNO in $(seq 1 2);
	do
		OBJCNT=1
		SIZE=$(( 1024 * 1024 * 1024 * SIZEGB ))
		vmobject $OBJCNT "$SIZE" "$RUNNO"
	done
done


# Get numbers from 8GB to 80GB, rising 8GB at a time. Use only one object.
for SIZEGB in $(seq 8 8 88);
do
	for RUNNO in $(seq 1 2);
	do
		OBJCNT=1
		SIZE=$(( 1024 * 1024 * 1024 * SIZEGB ))
		vmobject $OBJCNT "$SIZE" "$RUNNO"
	done
done

