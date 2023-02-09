#!/bin/sh

# Attempt to load the sls without a mounted FS. Should fail.

kldload slos
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

kldload sls 2>&1
if [ $? -eq 0 ]; then
    echo "Mounted the SLS without a file system"
    exit 1
fi

kldunload slos 2> /dev/null
if [ $? -ne 0 ]; then
    echo "Unloaded the SLOS while the SLSFS is mounted"
    exit 1
fi

exit 0
