#!/bin/sh

gstripe destroy st0
gstripe create -s 65536 -v st0 nvd0 nvd1 #nvd2 nvd3
gstripe destroy st0
gstripe create -s 65536 -v st0 nvd0 nvd1 #nvd2 nvd3

DRIVE=/dev/stripe/st0

#DRIVE=/dev/nvd0

./tools/newosd/newosd $DRIVE

kldload slos/slos.ko

mount -rw -t slsfs $DRIVE /testmnt
#./dd.sh

#fio trace/test.fio

#echo "hello" > /testmnt/hello
#kldload kmod/sls.ko
#filebench -f trace/varmail.f &2> /dev/null
##sleep 10
#filebench -f benchmarks/scripts/randomrw.f

#kldunload sls
#umount /testmnt
#kldunload slos

echo "hello" > /testmnt/hello.txt

umount /testmnt

mount -rw -t slsfs $DRIVE /testmnt


#mkdir -p /testmnt/dingdong/hello/2/

# Optimized newfs
# newfs -j -S 4096 -b 65536 $DRIVE

