#!/bin/sh

. aurora
aursetup

TESTDIR=$PWD

cd /testmnt

"$TESTDIR/walfd/walfd" "$MNT" > /dev/null 2> /dev/null
CODE=$?

cd $TESTDIR
if [ $CODE -ne 0 ];
then
    echo "Did not read back a sync'd write - $CODE"
    aurteardown
    exit 1
fi

cd $TESTDIR
aurteardown
