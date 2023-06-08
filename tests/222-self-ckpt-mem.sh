#!/bin/sh

. aurora
aursetup

"./selfie/selfie" > /dev/null 2> /dev/null &
PID=$!
wait $PID

slsrestore
PID=$!
wait $PID

RET=$?
if [ $RET -ne 0 ];
then
    echo "Process exited with nonzero $RET"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0


