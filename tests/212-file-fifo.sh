#!/bin/sh

FIFO="testfifo"
. aurora
aursetup

# Create it outside the process to put it in the name cache.
mkfifo $FIFO 2> /dev/null
sleep 1

"./fifo/fifo" $PWD > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    rm $FIFO
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 1
wait $PID

slsrestore
if [ $? -ne 0 ];
then
    rm $FIFO
    echo "Restore failed with $?"
    exit 1
fi

sleep 1

wait $!
if [ $? -ne 0 ];
then
    rm $FIFO
    echo "Process exited with $?"
    exit 1
fi

rm $FIFO

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
