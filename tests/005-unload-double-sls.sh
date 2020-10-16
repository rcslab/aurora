#!/bin/sh -e

# Attempt to unload the sls twice. Should fail.

. aurora

loadmod
unloadsls
checkfail "unloadsls"
unloadslos

exit 0
