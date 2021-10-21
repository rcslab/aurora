#!/bin/sh

# Attempt to unload the slos while the FS is still active. Should fail.

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

unloadsls
if [ $? -ne 0 ]; then
    echo "Failed to unload SLS"
    exit 1
fi


unloadslos 2>&1
if [ $? -eq 0 ]; then
    echo "Unloaded the SLOS with an FS mounted"
    exit 1
fi

slsunmount
if [ $? -ne 0 ]; then
    echo "Failed to unmount FS"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to remove the SLOS"
    exit 1
fi

exit 0
