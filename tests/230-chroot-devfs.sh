#!/bin/sh

# Test whether using the devfs on a chroot works properly.

. aurora
aursetup

installroot

cp ./fd/fd "/$MNT/fd"
chroot "/$MNT" "/fd" / > /dev/null 2> /dev/null &
PID=`pidof fd`
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    umount "$MNT/dev"
    exit 1
fi

sleep 1
wait $PID

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    umount "$MNT/dev"
    exit 1
fi

wait $!
if [ $? -ne 0 ];
then
    echo "Process exited with nonzero"
    umount "$MNT/dev"
    exit 1
fi

rm -f "$MNT/testfile"

sleep 3

aurteardown

exit 0
