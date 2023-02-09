#!/bin/sh

export OID=1000

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./array/array >/dev/null 2>/dev/null &
PID="$!"

slsctl partadd -o $OID -b slos -a 50
slsctl attach -o $OID -p $PID

slsctl checkpoint -o $OID
if [ $? -ne 0 ];
then
    echo "Checkpoint failed with $?"
    aurteardown
    exit 1
fi

sleep 1
killandwait $PID

# This call is supposed to fail. 
slsctl restore -o $OID >/dev/null 2>/dev/null
if [ $? -eq 0 ];
then
    echo "Restored amplified write"
    aurteardown
    exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
