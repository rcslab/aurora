#!/bin/sh

set -euo

TEST="array"
SLSCTL="tools/slsctl/slsctl"
OID="5"
BACKEND="slos"
SLEEP="10"

"$SLSCTL" partadd -o "$OID" -b "$BACKEND" -t 1000 -d
"$SLSCTL" attach -o "$OID" -p `pidof $TEST`
"$SLSCTL" checkpoint -o "$OID"

sleep "$SLEEP"

"$SLSCTL" restore -o "$OID"
