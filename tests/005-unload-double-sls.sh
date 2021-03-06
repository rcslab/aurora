#!/bin/sh

# Attempt to unload the sls twice. Should fail.

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

unloadsls
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLS"
    exit 1
fi

unloadsls 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Unloaded the SLS twice"
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
