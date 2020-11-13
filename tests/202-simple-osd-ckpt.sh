#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

dd if=/dev/zero of=/dev/null bs=1m 1>&2 &

slsosdcheckpoint `jobid %1`
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    aurteardown
    exit 1
fi

killandwait %1

slsosdrestore
if [ $? -ne 0 ] && [ $? -ne 3 ];
then
    echo "Restore failed with $?"
    aurteardown
    exit 1
fi

sleep 1
killandwait $!
pkill dd
sleep 1

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
