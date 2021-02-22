#!/bin/sh

. aurora

aursetup

./metropolis/metropolis -a 1>&2 &
PID=$!

# Wait for the run to finish. The restore happens internally.
wait $PID
if [ $? -ne 0 ];
then
    aurteardown > /dev/null 2> /dev/null
    echo "Process exited with nonzero"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi
