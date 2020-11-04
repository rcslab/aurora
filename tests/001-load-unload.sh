#!/bin/sh

# Normal load unload cycle without mounting.

. aurora

for i in `seq 3`
do
    loadmod
    if [ $? -ne 0 ]; then
	echo "Failed to load modules"
	exit 1
    fi

    unloadsls
    if [ $? -ne 0 ]; then
	echo "Failed to unload SLS"
	exit 1
    fi

    unloadslos
    if [ $? -ne 0 ]; then
	echo "Failed to unload SLOS"
	exit 1
    fi
done

exit 0
