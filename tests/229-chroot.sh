#!/bin/sh

. aurora
aursetup
installroot $MNT

# Like test 206 (file), but using the SLOS as a root filesystem.
cp "./mmap/mmap" "$MNT/mmap"
chroot "$MNT" "/mmap" / anon > /dev/null 2> /dev/null &
PID=$!
sleep 1

slscheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 1
wait $PID

slsrestore
if [ $? -ne 0 ];
then
    echo "Restore failed with $?"
    exit 1
fi

sleep 1

wait `pidof mmap`

EXIT=$?
if [ $EXIT -ne 0 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
