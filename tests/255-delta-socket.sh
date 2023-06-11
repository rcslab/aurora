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
slsctl partadd recv -o $RECVOID -P $PORT -A $ADDR -d
if [ $? -ne 0 ];
then
    echo "Recv partadd failed"
    aurteardown
    exit 1
fi

slsctl partadd send -o $SENDOID -P $PORT -A $ADDR -d -t 100
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

sleep 5
kill $PID
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
