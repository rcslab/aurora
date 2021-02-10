#!/bin/sh

. aurora
aursetup

"./udplisten/udplisten" > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

# Write out a message, keep the connected socket open.
printf "message\0" | nc -u localhost 6668 &

killandwait $PID
sleep 10

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi
PID=$!

sleep 1
printf "message\0" | nc -u -N localhost 6668 &
sleep 1
killandwait $!

wait $PID
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
