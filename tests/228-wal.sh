#!/bin/sh

. aurora
aursetup

# Let the workload snapshot itself and exit with an error.
"./wal/wal" >/dev/null 2>&1 &
PID=$!
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
