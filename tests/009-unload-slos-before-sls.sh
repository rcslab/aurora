#!/bin/sh

# Attempt to unload the slos before the SLS. Succeeds because the
# kernel link properly reference counts the SLOS module and removes
# it only after the SLS module is gone.

. aurora

loadmod
if [ $? -ne 0 ]; then
    echo "Failed to load the modules"
    exit 1
fi

unloadslos
if [ $? -ne 0 ]; then
    echo "Failed to unload the SLOS"
    exit 1
fi

unloadsls
if [ $? -ne 0 ]; then
    echo "Could not unload the SLOS"
    exit 1
fi

unloadslos 2> /dev/null
if [ $? -eq 0 ]; then
    echo "Unloaded the SLOS both before and after the SLS"
    exit 1
fi

exit 0
