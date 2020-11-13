#!/bin/sh

. aurora
aursetup

"./multithread/multithread" "$MNT" file > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
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
pkill multithread
killandwait $!
sleep 2

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
