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
if [ $(( $EXIT & 127 )) -ne 0 -a $EXIT -ne $(( 128 | 9 )) ];
then
    echo "Process exited with $EXIT, should have exited with 0 or SIG{KILL, TERM}"
    exit 1
fi
