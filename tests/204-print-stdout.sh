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
pkill print
wait $!
EXIT=$?
# 15, exiting with SIGTERM
if [ $EXIT -ne 0 -a $EXIT -ne 15 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit $?
