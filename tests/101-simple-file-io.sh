#!/bin/sh

. aurora

aursetup
if [ $? -ne 0 ]; then
    echo "Failed to set up Aurora"
    exit 1
fi

echo foo > $MOUNTPT/foo
rm $MOUNTPT/foo

dd if=/dev/random of=$MOUNTPT/bar bs=1m count=10 2> /dev/null
rm $MOUNTPT/bar

mkdir $MOUNTPT/foo
rmdir $MOUNTPT/foo

ln -s garbage_string:123 $MOUNTPT/bar
rm $MOUNTPT/bar
aurteardown
if [ $? -ne 0 ]; then
    echo "Failed to tear down Aurora"
    exit 1
fi

exit 0
