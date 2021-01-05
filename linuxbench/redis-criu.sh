#!/bin/sh

BASE="100"
STEPS="10"
RESULTSDIR="criu-results"

SIZE="$BASE"

mkdir -p $RESULTSDIR

while [ $SIZE -le $(( $BASE * $STEPS )) ] 
do
	./redis-criu-single.sh $SIZE > "$RESULTSDIR/stats.$SIZE"
	SIZE=$(( $SIZE + $BASE ))
done
