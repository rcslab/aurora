#!/bin/sh

DRIVE=/dev/vtbd1

./tools/newfs/newfs $DRIVE

kldload slos/slos.ko

mount -rw -t slsfs $DRIVE /testmnt

kldload kmod/sls.ko

