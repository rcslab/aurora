#!/bin/sh -e

# Attempt to unload the slos before the sls. Should fail.

. aurora

loadslos
slsmount
checkfail "unloadslos"
slsunmount
unloadslos

exit 0
