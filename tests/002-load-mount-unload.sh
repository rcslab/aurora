#!/bin/sh

# Full setup and teardown, including mount/unmount.

. aurora

for i in `seq 3`
do
    aursetup
    if [ $? -ne 0 ]; then
	echo "Failed to set up Aurora"
	exit 1
    fi

    aurteardown
    if [ $? -ne 0 ]; then
	echo "Failed to tear down Aurora"
	exit 1
    fi
done

exit 0
