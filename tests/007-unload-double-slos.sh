#!/bin/sh

# Attempt to unload the slos twice. Should fail.

. aurora

loadmod
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

unloadsls
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLS"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi

unloadslos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Unloaded the SLOS twice"
    exit 1
fi

exit 0
