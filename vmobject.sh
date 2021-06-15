#!/bin/sh

export SRCROOT=/root/sls

. $SRCROOT/tests/aurora

gstripe destroy st0
gstripe create -s 65536 st0 nvd0 nvd1 nvd2 nvd3
gstripe destroy st0
gstripe create -s 65536 st0 nvd0 nvd1 nvd2 nvd3

export DISK=stripe/st0
export DISKPATH=/dev/stripe/st0

round() {
    aursetup
    #$SRCROOT/scripts/ckpt.d &
    sleep 1
    $SRCROOT/tests/vmobject/vmobject 1 $1
    sleep 2
    #pkill -SIGTERM ckpt.d dtrace
    sleep 2
    aurteardown
}


pkill ckpt.d dtrace
aurteardown

# Hardcoded values 32MB - 1G
for i in 4096 16384 65536 262144 1048576 4194304 16777216 67108864 268435456 1073741824; do
    round $i
done

