#!/bin/sh

. aurora
aursetup

TESTDIR=$PWD

cd $MNT

"$TESTDIR/sas/sas" "$MNT" 1000
CODE=$?

cd $TESTDIR
if [ $CODE -ne 0 ];
then
    echo "Test SAS returned code $CODE"
    aurteardown
    exit 1
fi

cd $TESTDIR
aurteardown
