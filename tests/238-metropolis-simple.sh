#!/bin/sh

. aurora

aursetup

echo "$PWD"
./metrosimple/metrosimple 1>&2 &
PID=$!

wait $PID
RET=$?
if [ $RET -ne 0 ];
then
	echo "Metropolis mode failed with $RET"
	aurteardown 
	exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi
