#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./print/print > /dev/null 2> /dev/null &
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
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

sleep 1
killandwait $!
pkill print

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit $?
