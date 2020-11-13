#!/bin/sh

. aurora
aursetup

ORIGDIR=$PWD
cd "$MNT"

"$ORIGDIR/mmap/mmap" > $TESTLOG &
PID=$!
cd "$ORIGDIR"
sleep 1

slscheckpoint $PID
cd $ORIGDIR
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 1
killandwait $PID

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

sleep 1
killandwait $!  
sleep 5

if [ $? -ne 0 ];
then
    echo "Process exited with $?"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
