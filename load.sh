#!/bin/sh

./tools/newosd/newosd /dev/vtbd1

kldload slos/slos.ko

mount -rw -t slsfs /dev/vtbd1 /testmnt

#fio trace/test.fio

#echo "hello" > /testmnt/hello
kldload kmod/sls.ko
#filebench -f trace/varmail.f &2> /dev/null
##sleep 10
#filebench -f trace/randomrw.f &2> /dev/null

#kldunload sls
#umount /testmnt
#kldunload slos

#mount -rw -t slsfs /dev/vtbd1 /testmnt


#mkdir -p /testmnt/dingdong/hello/2/

