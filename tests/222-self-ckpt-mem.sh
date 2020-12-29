#!/bin/sh

. aurora
aursetup

"./selfie/selfie" > /dev/null 2> /dev/null &
PID=$!

wait $PID

# Wait for the memory checkpoint to become available
sleep 2

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

wait `pidof selfie`
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


