#!/bin/sh

. aurora

aursetup
dd if=/dev/zero of=/dev/null bs=1m 1>&2 &

OID="4567"
slslazycheckpoint `jobid %1` $OID
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    exit 1
fi

killandwait %1

slslazyrestore $OID
if [ $? -ne 0 ];
then
    echo "Restore failed"
    aurteardown
    exit 1
fi

sleep 1
killandwait $!


aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
