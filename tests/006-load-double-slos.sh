#!/bin/sh -e

# Attempt to load the slos twice. Should fail.

. aurora

loadmod
checkfail "loadslos"
unloadmod

exit 0
