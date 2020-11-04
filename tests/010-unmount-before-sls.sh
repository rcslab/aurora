#!/bin/sh

# Attempt to unload the slos before the sls. Should fail.

. aurora

loadslos
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

slsmount
if [ $? -ne 0 ]; then
    echo "Failed to mount the SLSFS"
    exit 1
fi

unloadslos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Unloaded the SLOS while the SLSFS is mounted"
    exit 1
fi

slsunmount
if [ $? -ne 0 ]; then
    echo "Failed to unmount the SLSFS"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi


exit 0
