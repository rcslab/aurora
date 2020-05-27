#!/bin/sh

gstripe destroy st0
gstripe create -s 65536 -v st0 vtbd1 vtbd2 vtbd3
gstripe destroy st0
gstripe create -s 65536 -v st0 vtbd1 vtbd2 vtbd3
#gstripe create -s 65536 -v st0 nvd0 nvd1 nvd2 nvd3


DRIVE=/dev/stripe/st0

#DRIVE=/dev/vtbd1

./tools/newosd/newosd $DRIVE

kldload slos/slos.ko

mount -rw -t slsfs $DRIVE /testmnt

filebench -f benchmarks/scripts/randomrw.f
echo "hello" > /testmnt/hello.txt

echo "unmount"
umount /testmnt

kldunload slos/slos.ko
kldload slos/slos.ko


echo "remount"
mount -rw -t slsfs $DRIVE /testmnt


#filebench -f benchmarks/scripts/randomrw.f

