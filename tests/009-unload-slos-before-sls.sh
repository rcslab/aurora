#!/bin/sh -e

# Attempt to unload the slos before the SLS. Succeeds because the
# kernel link properly reference counts the SLOS module and removes
# it only after the SLS module is gone.

. aurora

loadslos
loadsls
unloadslos
unloadsls

exit 0
