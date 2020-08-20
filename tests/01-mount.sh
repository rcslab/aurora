#!/bin/sh -e

. aurora

for i in `seq 2`
do
	slsunmount
	sleep 1
	slsmount
	sleep 1
done

exit 0
