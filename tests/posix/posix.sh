#!/usr/local/bin/bash

SLSDIR="/root/sls"
BIN="/$SLSDIR/tests/posix/posix"

source "$SLSDIR/scripts/bench.sh"

aurstripe
aurload

dtrace "$SLSDIR/scripts/posix.d" > "$SLSDIR/posix.csv" &
"$BIN" "$MOUNTDIR"
pkill -SIGTERM dtrace
sleep 2

aurunload
