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

slsosdcheckpoint $PID
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    exit 1
fi

sleep 1
killandwait $PID

kldunload metropolis
kldunload sls
kldload sls
kldload metropolis

slsosdrestore
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

REST=$!

sleep 1

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
