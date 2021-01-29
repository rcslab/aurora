#!/bin/sh

# Get the global configs.
SRCROOT=../..
. $SRCROOT/tests/aurora
export SRCROOT

createmd

export MNT
export DISK
export DISKPATH

BASE="100"
STEPS="10"
RESULTSDIR="aurora-results"

SIZE="$BASE"

# Clean up any Aurora state. Aurora is set up from scratch on every iteration.
aurteardown 1>/dev/null 2>/dev/null

# We use DTrace to measure performance.
mkdir -p $RESULTSDIR

while [ $SIZE -le $(( $BASE * $STEPS )) ]
do
	./redis-aurora-single.sh $SIZE > "$RESULTSDIR/stats.$SIZE"
	SIZE=$(( $SIZE + $BASE ))
	aurteardown 1>/dev/null 2> /dev/null
done

# Clean up after we're done.
aurteardown 1> /dev/null 2> /dev/null

destroymd $MDDISK
