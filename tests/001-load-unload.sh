#!/bin/sh

# Normal load unload SLOS cycle without mounting.

. aurora

for i in `seq 3`
do
    kldload slos
    if [ $? -ne 0 ]; then
	echo "Failed to load modules"
	exit 1
    fi

    kldunload slos
    if [ $? -ne 0 ]; then
	echo "Failed to unload SLOS"
	exit 1
    fi
done

exit 0
