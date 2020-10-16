#!/bin/sh -e

# Attempt to unload the slos twice. Should fail.

. aurora

loadmod
unloadsls
unloadslos
checkfail "unloadslos"

exit 0
