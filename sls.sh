#!/bin/sh

set -euo

TEST="llist"
SLSCTL="tools/slsctl/slsctl"
OID="5"
BACKEND="memory"
SLEEP="3"

"$SLSCTL" partadd -o "$OID" -b "$BACKEND" 
"$SLSCTL" attach -o "$OID" -p `pidof $TEST`
"$SLSCTL" checkpoint -o "$OID"

sleep "$SLEEP"

"$SLSCTL" restore -o "$OID"
