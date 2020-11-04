#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

for i in `seq 3`
do
	slsunmount
	if [ $? -ne 0 ]; then
	    echo "Failed to unmount the SLSFS"
	    exit 1
	fi

	slsmount
	if [ $? -ne 0 ]; then
	    echo "Failed to remount the SLSFS"
	    exit 1
	fi
done

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi


