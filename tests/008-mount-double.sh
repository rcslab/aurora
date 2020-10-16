#!/bin/sh -e

# Attempt to unload the slos twice. Should fail.

. aurora

loadmod
slsmount
checkfail "slsmount"
slsunmount
modunload

exit 0
