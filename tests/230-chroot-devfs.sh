#!/bin/sh

# Test whether using the devfs on a chroot works properly.

. aurora
aursetup

installminroot
mount -t devfs devfs "$MNT/dev"

cp $SRCROOT/tests/fd/fd "/$MNT/fd"
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

#slsrestore
#if [ $? -ne 0 ];
#then
#    echo "Restore failed with $?"
#    umount "$MNT/dev"
#    exit 1
#fi
#
#wait $!
#if [ $? -ne 0 ];
#then
#    echo "Process exited with nonzero"
#    umount "$MNT/dev"
#    exit 1
#fi

rm -f "$MNT/testfile"

#The following commands have to be done in order to unload properly.

# Unload the SLS module first so that all vnode references are released. 
unloadsls
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

umount "$MNT/dev"
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

slsunmount
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
