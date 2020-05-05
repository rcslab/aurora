#!/bin/sh

gstripe destroy st0
setup_stripe.sh
gstripe destroy st0
setup_stripe.sh

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
#filebench -f trace/randomrw.f &2> /dev/null

#kldunload sls
#umount /testmnt
#kldunload slos

#mount -rw -t slsfs /dev/vtbd1 /testmnt


#mkdir -p /testmnt/dingdong/hello/2/

