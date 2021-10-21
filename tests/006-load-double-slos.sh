#!/bin/sh

# Attempt to load the slos twice. Should fail.

. aurora

loadslos
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

loadslos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Loaded the SLOS module twice"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to unload the modules"
    exit 1
fi

exit 0
