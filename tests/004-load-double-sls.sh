#!/bin/sh -e

# Attempt to load the sls twice. Should fail.

. aurora

loadmod
checkfail "loadsls"
unloadmod

exit 0
