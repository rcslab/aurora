#!/bin/sh

OID=4545
export SRCROOT=/root/sls

. $SRCROOT/tests/aurora


gstripe destroy st0
gstripe create -s 65536 st0 nvd0 nvd1 nvd2 nvd3
gstripe destroy st0
gstripe create -s 65536 st0 nvd0 nvd1 nvd2 nvd3

export DISK=/stripe/st0
export DISKPATH=/dev/stripe/st0

cleanup()
{
    pkill posix.d dtrace
    sleep 1
    aurteardown
}

round() {
    aursetup
    $SRCROOT/scripts/posix.d &
    sleep 1
    $SRCROOT/tests/posix/posix $OID $MNT
    sleep 2
    $SRCROOT/tools/slsctl/slsctl restore -o $OID
    sleep 2
    cleanup
}


cleanup
round
