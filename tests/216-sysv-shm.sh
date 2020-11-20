#!/bin/sh

. aurora
aursetup

"./sysvshm/sysvshm" > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 1
# The segment is destroyed by the test, recreated by the SLS.
wait $PID

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
    echo "Process exited with $?"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
