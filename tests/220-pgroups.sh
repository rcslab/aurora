#!/bin/sh

. aurora
aursetup

./pgroup/pgroup & # > /dev/null 2> /dev/null &
PID=$!
echo $PID
sleep 5

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 2
pkill `pidof pgroup`
wait $PID
sleep 2

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

sleep 1

wait $!
if [ $? -ne 0 ];
then
    echo "Process exited with nonzero"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
