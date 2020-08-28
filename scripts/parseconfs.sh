#/usr/local/bin/bash

source 

LEADINGNEWLINE="sed '%s/^\s\+//e"
TRAILINGNEWLINE="sed '%s/\s\+$//e"
REDISCONFOUT=""

cat $REDISCONF | `LEADINGNEWLINE` | `TRAILINGNEWLINE` | \
    > $REDISCONFOUT
