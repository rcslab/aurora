#!/bin/sh

# Attempt to load the SLS twice. Should fail.

. aurora

loadmod
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

loadsls 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Remounted the SLS twice"
    exit 1
fi

unloadmod
if [ $? -ne 0 ]; then
    echo "Failed to unload the modules"
    exit 1
fi


exit 0
