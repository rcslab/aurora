#!/bin/sh

. aurora

kldload slos
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

slsnewfs
if [ $? -ne 0 ]; then
    echo "Failed to create the SLSFS"
    exit 1
fi

slsmount
if [ $? -ne 0 ]; then
    echo "Failed to mount Aurora"
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

slsunmount
if [ $? -ne 0 ]; then
    echo "Failed to unmount the SLSFS"
    exit 1
fi

kldunload slos
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi


