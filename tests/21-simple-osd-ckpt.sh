#!/bin/sh

. aurora

aursetup
dd if=/dev/zero of=/dev/null bs=1m 1>&2 &

slsosdcheckpoint `jobid %1`
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    exit 1
fi 

killandwait %1

slsosdrestore
if [ $? -ne 0 ];
then
    echo Restore failed
    aurteardown
    exit 1
fi 

sleep 1
killall $! 2> /dev/null
killall dd 2> /dev/null
aurteardown

exit 0
