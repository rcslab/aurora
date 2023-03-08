#!/bin/sh

SENDOID=10101
RECVOID=10102
PORT=5040
ADDR="127.0.0.1"

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1

# XXX Partadd for the receive
slsctl partadd recv -o $RECVOID -P $PORT -A $ADDR
if [ $? -ne 0 ];
then
    echo "Recv partadd failed"
    aurteardown
    exit 1
fi

slsctl partadd send -o $SENDOID -P $PORT -A $ADDR
if [ $? -ne 0 ];
then
    echo "Send partadd failed"
    aurteardown
    exit 1
fi

slsctl attach -p $PID -o $SENDOID
if [ $? -ne 0 ];
then
    echo "Attach failed"
    aurteardown
    exit 1
fi

slsctl checkpoint -o $SENDOID -r
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    exit 1
fi

sleep 2
killandwait $PID
sleep 2

echo "Restoring $RECVOID"
slsctl restore -o $RECVOID &
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

REST=$!

sleep 3

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
