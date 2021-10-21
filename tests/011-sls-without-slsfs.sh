#!/bin/sh

# Attempt to load the sls without a mounted FS. Should fail.

. aurora

loadslos
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

loadsls 2>&1
if [ $? -eq 0 ]; then
    echo "Mounted the SLS without a file system"
    exit 1
fi

unloadslos 2> /dev/null
if [ $? -ne 0 ]; then
    echo "Unloaded the SLOS while the SLSFS is mounted"
    exit 1
fi

exit 0
