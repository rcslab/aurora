#!/bin/sh

. aurora

aursetup

# Redirect to /dev/null, we don't care about stdin.
./compute/compute > /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    exit 1
fi

killandwait $PID

slsrestore
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi


sleep 1
killandwait $!
pkill compute

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
