#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

dd if=/dev/zero of=/dev/null bs=1m 1>&2 &

slscheckpoint `jobid %1`
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

killandwait %1

dtrace -s $SRCROOT/scripts/rest.d &
PID=$!
sleep 2

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

sleep 1
killandwait $! 2> /dev/null
killall dd
pkill dtrace
wait $!
sleep 2

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
