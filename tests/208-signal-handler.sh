#!/bin/sh

. aurora
aursetup

"./signal/signal" > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed"
    exit 1
fi

sleep 1
kill -SIGUSR1 $PID
killandwait $PID

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed"
    exit 1
fi

sleep 2
pkill -SIGUSR1 signal
wait $!

EXIT=$?
if [ $EXIT -ne 0 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
