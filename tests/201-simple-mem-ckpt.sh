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

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

sleep 1
killandwait $! 2> /dev/null
killall dd

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
