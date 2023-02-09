#!/bin/sh

# Attempt to load the slos twice. Should fail.

. aurora

kldload slos
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

kldload slos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Loaded the SLOS module twice"
    exit 1
fi

kldunload slos
if [ $? -ne 0 ]; then
    echo "Failed to unload the modules"
    exit 1
fi

exit 0
