#!/bin/sh

CKPTDIR="/ckptdir"
SOCKOID=10101
FILEOID=10102
PORT=5040
ADDR="127.0.0.1"

. aurora

# See test 250 for details.
ffssetup()
{
	MD=`mdconfig -a -t malloc -s 1g`
	newfs "/dev/$MD"
	mount -t ufs "/dev/$MD" $CKPTDIR 
}

ffsteardown()
{
	umount $CKPTDIR
	mdconfig -d -u $MD
	rm -r $CKPTDIR
}


aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi


# Clean up file descriptor
rm -rf $CKPTDIR
mkdir -p $CKPTDIR

ffssetup

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1

# Start up the server
$SRCROOT/tools/server/server $CKPTDIR &
SERVER=$!
sleep 2

slsctl partadd send -o $SOCKOID -P $PORT -A $ADDR
if [ $? -ne 0 ];
then
    echo "Partadd failed"
    aurteardown
    ffsteardown
    exit 1
fi

slsctl attach -p $PID -o $SOCKOID
if [ $? -ne 0 ];
then
    echo "Attach failed"
    aurteardown
    ffsteardown
    exit 1
fi

slsctl checkpoint -o $SOCKOID -r
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    ffsteardown
    exit 1
fi

sleep 1
killandwait $PID

# Kill the server
killandwait $SERVER

echo "Restoring $FILEOID"
slsctl restore -o $FILEOID &
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    exit 1
fi

REST=$!

sleep 3

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

ffsteardown

exit 0
