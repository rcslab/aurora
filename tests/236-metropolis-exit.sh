#!/bin/sh

. aurora

aursetup

./metropolis/metropolis -x 1>&2 &
PID=$!

sleep 2

# Tear down the module. This should kill the process.
aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

# Wait for the run to finish. The restore happens internally.
wait $PID
EXIT=$?
if [ $EXIT -ne 128 ];
then
    echo "Process exited with $EXIT, should have exited with 128 (SIGKILL)"
    exit 1
fi
