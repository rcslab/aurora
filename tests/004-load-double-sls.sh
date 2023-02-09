#!/bin/sh

# Attempt to load the SLS twice. Should fail.

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

kldload sls 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Remounted the SLS twice"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi


exit 0
