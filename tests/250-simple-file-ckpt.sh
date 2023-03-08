#!/bin/sh

CKPTDIR="/ckptdir"
OID=10101

. aurora

# We need to store the checkpoints in a file system like FFS 
# that has ioctl() calls for seeking holes and data.
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

# Clean up mount directory
rm -rf $CKPTDIR
mkdir $CKPTDIR

ffssetup

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1


slsctl partadd file -o $OID -f $CKPTDIR
if [ $? -ne 0 ];
then
    echo "Partadd failed"
    aurteardown
    ffsteardown	
    exit 1
fi

slsctl attach -p $PID -o $OID
if [ $? -ne 0 ];
then
    echo "Attach failed"
    aurteardown
    ffsteardown	
    exit 1
fi

slsctl checkpoint -o $OID -r
if [ $? -ne 0 ];
then
    echo Checkpoint failed
    aurteardown
    ffsteardown	
    exit 1
fi

sleep 1
killandwait $PID

slsctl restore -o $OID &
# Killing the workload using a signal makes the restore exit with 3.
if [ $? -ne 0 ];
then
    echo Restore failed
    aurteardown
    ffsteardown	
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

ffsteardown	

exit 0
