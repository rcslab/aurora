#!/bin/sh

# Attempt to unmount with a running SLS. Should fail.

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

slsunmount 2>&1
if [ $? -eq  0 ]; then
    echo "Unmounted with the SLS running"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi


exit 0
