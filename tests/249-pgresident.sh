#!/bin/sh

. aurora

OID=1600
FILENAME="pgresident"

rm -f $FILENAME

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

slsunmount 2>&1

./array/array > /dev/null 2> /dev/null &
PID=$!
sleep 1

slsctl partadd slos -o $OID
RET=$?
if [ $RET -ne 0 ];
then
    echo "Adding partition failed with $RET"
    exit 1
fi

slsctl attach -o $OID -p `jobid %1`
RET=$?
if [ $? -ne 0 ];
then
    echo "Attaching to partition failed with $RET"
    exit 1
fi

slsctl pgresident -o $OID -f $FILENAME
RET=$?
if [ $? -ne 0 ];
then
    echo "Page resident counting failed with $RET"
    exit 1
fi

killandwait %1

#rm -f $FILENAME

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
