#!/bin/sh

# Test whether using the devfs on a chroot works properly.

. aurora

# Load the SLOS and mount the SLSFS
loadslos
slsnewfs
slsmount

# Mount the devfs
mkdir -p $MNT/dev
mount -t devfs devfs $MNT/dev

# Create the root
installroot

# Copy over the workload and run it
cp $SRCROOT/tests/fd/fd "/$MNT/fd"
chroot "/$MNT" "/fd" / > /dev/null 2> /dev/null &
PID=`pidof fd`

#  Wait for a bit then kill it
sleep 2
pkill fd
sleep 2

# Remove the devfs, the slsfs, and the slos, in that order
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
