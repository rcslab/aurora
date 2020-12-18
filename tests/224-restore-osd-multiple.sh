#!/bin/sh

. aurora
aursetup

"./mmap/mmap" "$MNT" anon > /dev/null 2> /dev/null &
PID=$!
sleep 1

slsosdcheckpoint $PID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    exit 1
fi

sleep 1
wait $PID

for i in 1 2 3 4 5; do
    slsosdrestore
    if [ $? -ne 0 ];
    then
	echo "Restore failed with $?"
	exit 1
    fi

    wait $!
    if [ $? -ne 0 ];
    then
	echo "Process exited with nonzero"
	exit 1
    fi
done

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0


