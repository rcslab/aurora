#!/bin/sh -e

. aurora

aursetup
for i in `seq 3`
do
	slsunmount
	slsmount
done
aurteardown

exit 0
