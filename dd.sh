#!/bin/sh
DEV=/testffs
touch $DEV/dd1
touch $DEV/dd2
touch $DEV/dd3
touch $DEV/dd4

dd if=/dev/zero of=$DEV/dd1 &
dd if=/dev/zero of=$DEV/dd2 &
dd if=/dev/zero of=$DEV/dd3 &
dd if=/dev/zero of=$DEV/dd4 &
