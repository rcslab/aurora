#!/bin/sh

gstripe create -s 16384 -v st0 /dev/nvd0 /dev/nvd1 /dev/nvd2 /dev/nvd3
DISK="/dev/stripe/st0"
./tools/newosd/newosd "$DISK"

kldload slos/slos.ko

mount -rw -t slsfs "$DISK" /testmnt

#fio trace/test.fio

#echo "hello" > /testmnt/hello
#kldload kmod/sls.ko
#filebench -f trace/varmail.f &2> /dev/null
##sleep 10
#filebench -f trace/randomrw.f &2> /dev/null

#kldunload sls
#umount /testmnt
#kldunload slos

#mount -rw -t slsfs /dev/vtbd1 /testmnt


#mkdir -p /testmnt/dingdong/hello/2/

