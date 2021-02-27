#!/bin/sh

. aurora

aursetup

echo "$PWD"
./pymetro-partition/pymetro-partition  1>&2 &
PID=$!

wait $PID
if [ $? -ne 0 ];
then
	echo "Metropolis mode failed"
	aurteardown 
	exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi