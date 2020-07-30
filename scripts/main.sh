#!/usr/bin/env bash

set -euo

# Load the necessary script functions.
SCRIPTDIR="/root/sls/scripts/"
source "$SCRIPTDIR/aurora.sh"
source "$SCRIPTDIR/redis.sh"

echo "Starting"
echo "Loading Aurora..."
aurload

echo "Starting Redis server..."
rdstart

sleep 2
echo "Checkpointing Redis server..."

slsckpt `pidof "$SERVER"`

sleep 4

echo "Killing the server..."
rdstop

sleep 2

echo "Cleaning up Aurora..."
aurunload

echo "Done."
