#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    exit 1
fi

sleep 1
killandwait $PID

slsrestore
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

sleep 1
killandwait $!

pkill array

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit $?