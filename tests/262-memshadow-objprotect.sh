#!/bin/sh

. aurora
aursetup

sleep 1

# Let the workload snapshot itself and exit with an error.
sysctl aurora.objprotect=1
sysctl aurora.tracebuf=0
./memshadow/memshadow 

RET=$?
if [ $RET -ne 0 ];
then
    echo "Process exited with $RET"
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0


