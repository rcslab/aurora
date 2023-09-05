#!/bin/sh

OID=1055

. aurora
aursetup

sleep 3

# Let the workload snapshot itself and exit with an error.
"./memsnap/memsnap" -s > /dev/null 2 >/dev/null &
PID=$!
sleep 1

wait $PID

slsctl restore -o $OID &
PID=$!
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

# Get the error value, it should be zero.
wait $PID
RET=$?
if [ $RET -ne 0 ];
then
    echo "Process exited with $RET"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0


