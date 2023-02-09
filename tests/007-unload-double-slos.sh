#!/bin/sh

# Attempt to unload the slos twice. Should fail.

. aurora

kldload slos
if [ $? -ne 0 ]; then
    echo "Failed to load the SLOS"
    exit 1
fi

kldunload slos
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi

kldunload slos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Unloaded the SLOS twice"
    exit 1
fi

exit 0
