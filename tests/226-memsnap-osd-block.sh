#!/bin/sh

. aurora
aursetup

sleep 3

# Let the workload snapshot itself and exit with an error.
"./memsnap/memsnap" -w > /dev/null 2> /dev/null &
PID=$!
sleep 1

wait $PID

slsosdrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

# Get the error value, it should be zero.
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


