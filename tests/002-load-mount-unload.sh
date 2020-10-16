#!/bin/sh -e

# Normal load unload cycle without mounting.

. aurora

for i in `seq 3`
do
    aursetup
    aurteardown
done

exit 0
