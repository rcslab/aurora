#!/bin/sh

# Attempt to unload the slos twice. Should fail.

. aurora

aurstripe 2> /dev/null > /dev/null
if [ $? -ne 0 ]; then
    echo "Stripe failed"
    exit 1
fi

aursetup 2> /dev/null > /dev/null
if [ $? -ne 0 ]; then
    echo "Aurora setup failed"
    exit 1
fi

kldunload sls 2> /dev/null > /dev/null

GIGFILE=/tmp/gigfile

dd if=/dev/urandom of=$GIGFILE bs=1G count=1 > /dev/null 2> /dev/null

cp -r $SRCROOT $MNT/src
cp $GIGFILE $MNT/
ln -s $MNT/$(basename $GIGFILE) $MNT/gigfileln


slsunmount 2> /dev/null
if [ $? -ne 0 ]; then
    echo "Failed to unmount the SLSFS"
    exit 1
fi

slsmount 2> /dev/null
if [ $? -ne 0 ]; then
    echo "Failed to mount"
    exit 1
fi

diff $SRCROOT $MNT/src

if [ $? -ne 0 ]; then
    echo "Diff after remount corrupt"
    exit 1
fi

diff $GIGFILE $MNT/$(basename $GIGFILE)
if [ $? -ne 0 ]; then
    echo "Diff after remount corrupt"
    exit 1
fi

if [ `readlink $MNT/gigfileln` != "$MNT/$(basename $GIGFILE)" ]; then
    echo "Bad link"
    exit 1
fi


aurteardown

rm $GIGFILE

exit 0
