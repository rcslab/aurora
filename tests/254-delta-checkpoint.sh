#!/bin/sh

. aurora

OID=1000

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

slsctl partadd slos -o $OID -d

./delta/delta >/dev/null 2>/dev/null &

PID=`jobid %1`

sleep 1
slsctl attach -o $OID -p $PID
RET=$?
if [ $RET -ne 0 ];
then
    echo "attach failed with $RET"
    aurteardown
    exit 1
fi

for i in `seq 0 4`;
do
	# Full checkpoint
	slsctl checkpoint -o $OID
	RET=$?
	if [ $RET -ne 0 ];
	then
	    echo "checkpoint failed with $RET"
	    aurteardown
	    exit 1
	fi
done 

sleep 1

killandwait $PID

slsctl restore -o $OID &
RET=$?
if [ $RET -ne 0 ] && [ $RET -ne 3 ];
then
    echo "Restore failed with $RET"
    aurteardown
    exit 1
fi

REST=$!

sleep 1

pkill $REST

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

wait $REST
EXIT=$?
if [ $EXIT -ne 0 -a $EXIT -ne 9 ];
then
    echo "Process exited with $EXIT"
    exit 1
fi

exit 0
