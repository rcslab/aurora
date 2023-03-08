#!/bin/sh

restore_and_wait() {
    slsctl restore -o $OID &
    if [ $? -ne 0 ] && [ $? -ne 3 ];
    then
  echo "Restore failed with $?"
  aurteardown
  exit 1
    fi

    sleep 1
    pkill array
    sleep 2
}

export OID=1000

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

./array/array >/dev/null 2>/dev/null &
PID="$!"

slsctl partadd slos -o $OID -c -l -i
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

restore_and_wait
if [ $? -ne 0 ]; then
  echo "Failed initial restore"
  aurteardown
  exit 1
fi

restore_and_wait
if [ $? -ne 0 ]; then
  echo "Failed second restore"
  aurteardown
  exit 1
fi

aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
