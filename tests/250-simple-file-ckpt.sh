#!/bin/sh

CKPTDIR="$HOME/fileckpt"
OID=10101

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1

# Clean up file descriptor
rm -rf $CKPTDIR
mkdir $CKPTDIR

slsctl partadd file -o $OID -f $CKPTDIR
if [ $? -ne 0 ];
then
    echo "Partadd failed"
    aurteardown
    rmdir $CKPTDIR
    exit 1
fi

slsctl attach -p $PID -o $OID
if [ $? -ne 0 ];
then
    echo "Attach failed"
    aurteardown
    rmdir $CKPTDIR
    exit 1
fi

slsctl checkpoint -o $OID -r
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    rmdir $CKPTDIR
    exit 1
fi

sleep 1
killandwait $PID

slsctl restore -o $OID &
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

REST=$!

sleep 1

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

wait $REST
EXIT=$?
if [ $EXIT -ne 0 -a $EXIT -ne 9 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

# Clean up file descriptor
rm -r $CKPTDIR

exit 0
