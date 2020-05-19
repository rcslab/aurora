#!/bin/sh

#gstripe destroy st0
#gstripe create -s 1048576 -v st0 vtbd1 vtbd2 vtbd3
#gstripe create -s 65536 -v st0 nvd0 nvd1 nvd2 nvd3
#gstripe destroy st0
#gstripe create -s 65536 -v st0 nvd0 nvd1 nvd2 nvd3
#gstripe create -s 1048576 -v st0 vtbd1 vtbd2 vtbd3
set -euo

DRIVE=/dev/vtbd1

#DRIVE=/dev/vtbd1
make -j5 -DWITH_DFLAGS -DTEST

./tools/newosd/newosd $DRIVE

kldload slos/slos.ko

mount -rw -t slsfs $DRIVE /testmnt

kldload kmod/sls.ko

#fio trace/test.fio

#echo "hello" > /testmnt/hello
#kldload kmod/sls.ko
#filebench -f benchmarks/scripts/mongo.f
##sleep 10
#filebench -f trace/randomrw.f &2> /dev/null

#kldunload sls
#umount /testmnt
#kldunload slos

#mount -rw -t slsfs /dev/vtbd1 /testmnt


#mkdir -p /testmnt/dingdong/hello/2/

