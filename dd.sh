#!/bin/sh
touch /testmnt/dd1
touch /testmnt/dd2
touch /testmnt/dd3
touch /testmnt/dd4

dd if=/dev/zero of=/testmnt/dd1 &
dd if=/dev/zero of=/testmnt/dd2 &
dd if=/dev/zero of=/testmnt/dd3 &
dd if=/dev/zero of=/testmnt/dd4 &
