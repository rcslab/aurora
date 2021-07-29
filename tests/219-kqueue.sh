#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./kqueue/kqueue > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    exit 1
fi

sleep 1
wait $PID

slsrestore
## Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

REST=$!

sleep 2

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

wait $REST
EXIT=$?
if [ $EXIT -ne 0 -a $EXIT -ne 9 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

exit 0
