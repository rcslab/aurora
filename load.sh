#!/bin/sh

./tools/newosd/newosd /dev/vtbd1

kldload slos/slos.ko.full
kldload kmod/sls.ko.full
kldload slsfs/slsfs.ko.full

mount -rw -t slsfs /dev/vtbd1 /testmnt

#fio trace/test.fio

filebench -f trace/randomrw.f &2> /dev/null
#mkdir -p /testmnt/dingdong/hello/2/

